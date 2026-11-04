"""tools/quest_store_build.py (docs/adr/0042): the input checks, the cache key, the cache check and the collect step.

Everything runs on a scratch tree (quest_store_fixtures.py): the steps' outputs are made-up files, and the tool is called as the
recipe calls it. The real run (the store APK, the store OBB and a build of the sentinel) is `just quest-store-build`.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "tests"))
import quest_store_fixtures as fx  # noqa: E402

qsb = fx.qsb


def run_logged(fn, *args):
    """Calls fn and returns (result, the JSON lines it wrote)."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        result = fn(*args)
    return result, [json.loads(line) for line in out.getvalue().splitlines()]


class StoreBuildCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = pathlib.Path(self.tmp.name)
        self.layout, self.apk = fx.make_root(self.base)

    def collect(self, recipe=fx.RECIPE, signer=fx.SIGNER):
        return run_logged(qsb.collect, self.layout, self.apk, recipe, signer)

    def check(self, recipe=fx.RECIPE, signer=fx.SIGNER):
        return run_logged(qsb.check, self.layout, self.apk, recipe, signer)


class RequireTest(unittest.TestCase):
    def test_an_unset_variable_is_refused_by_name(self):
        for kind, variable in (("obb", "NEVR_STORE_OBB"), ("apk", "NEVR_STORE_APK")):
            with self.assertRaises(qsb.Refused) as caught:
                qsb.require(qsb.Layout(pathlib.Path("/nonexistent")), [kind], {})
            self.assertEqual(caught.exception.code, "input_unset")
            self.assertIn(variable, caught.exception.detail)

    def test_a_variable_that_is_not_a_file_is_refused_by_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(qsb.Refused) as caught:
                qsb.require(qsb.Layout(pathlib.Path(tmp)), ["obb"], {"NEVR_STORE_OBB": tmp})
            self.assertEqual(caught.exception.code, "input_not_a_file")
            self.assertIn("NEVR_STORE_OBB", caught.exception.detail)

    def test_files_and_a_keystore_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            obb = pathlib.Path(tmp) / "x.obb"
            obb.write_bytes(b"12345")
            result, lines = run_logged(qsb.require, qsb.Layout(pathlib.Path(tmp)), ["obb", "keystore"], {"NEVR_STORE_OBB": str(obb)},
                                       lambda layout: fx.SIGNER)
        self.assertEqual(result, 0)
        self.assertEqual([l["event"] for l in lines], ["input_ok", "input_ok"])
        self.assertEqual(lines[0]["bytes"], 5)
        self.assertEqual(lines[1]["signer_sha256"], fx.SIGNER)

    def test_the_command_line_refuses_with_a_json_line_and_exit_1(self):
        env = {k: v for k, v in os.environ.items() if k not in ("NEVR_STORE_OBB", "NEVR_STORE_APK")}
        done = subprocess.run([sys.executable, "-I", str(REPO / "tools" / "quest_store_build.py"), "require", "obb"], capture_output=True, text=True, env=env)
        self.assertEqual(done.returncode, 1)
        last = json.loads(done.stdout.strip().splitlines()[-1])
        self.assertEqual((last["tag"], last["event"], last["level"], last["code"]), ("NEVR.QUESTBUILD", "refused", "error", "input_unset"))
        self.assertIn("NEVR_STORE_OBB", last["detail"])


class CollectTest(StoreBuildCase):
    def test_collect_places_the_outputs_and_writes_the_manifest(self):
        status, lines = self.collect()
        self.assertEqual(status, 0)
        self.assertEqual([l["event"] for l in lines], ["outputs_placed", "data_matches_pins", "store_build_ready"])
        for line in lines:
            self.assertEqual((line["tag"], line["schema"]), ("NEVR.QUESTBUILD", 1))
        out = self.layout.out
        self.assertEqual((out / qsb.SIGNED_APK_NAME).read_bytes(), self.layout.repacked_apk.read_bytes())
        for path, content in fx.DATA.items():
            self.assertEqual((out / path).read_bytes(), content)
        manifest = json.loads(self.layout.manifest.read_text())
        self.assertEqual(manifest["outputs"]["apk"]["sha256"], fx.sha(b"signed apk made by the repack"))
        self.assertEqual({e["path"] for e in manifest["outputs"]["data"]}, set(fx.DATA))
        for entry in manifest["outputs"]["data"]:
            self.assertEqual(entry["sha256"], fx.sha(fx.DATA[entry["path"]]))
        self.assertEqual(manifest["inputs"]["stock_data"]["obb_sha256"], fx.OBB_SHA)
        self.assertEqual(manifest["inputs"]["store_apk"]["name"], "store.apk")
        self.assertNotIn(str(self.base), self.layout.manifest.read_text())  # no path of this machine in an output

    def test_the_data_is_linked_to_the_cache_not_copied(self):
        self.collect()
        stock = self.layout.stock_root / fx.OBB_SHA / next(iter(fx.DATA))
        placed = self.layout.out / next(iter(fx.DATA))
        self.assertTrue(os.path.samefile(stock, placed))

    def test_a_tree_that_differs_from_the_pins_is_refused_and_leaves_no_manifest(self):
        victim = self.layout.stock_root / fx.OBB_SHA / next(iter(fx.DATA))
        victim.write_bytes(b"x" * len(fx.DATA[next(iter(fx.DATA))]))  # same size, other bytes
        with self.assertRaises(qsb.Refused) as caught:
            self.collect()
        self.assertEqual(caught.exception.code, "pin_digest_mismatch")
        self.assertIn(next(iter(fx.DATA)), caught.exception.detail)
        self.assertFalse(self.layout.manifest.exists())

    def test_a_size_that_differs_from_the_pins_is_refused(self):
        (self.layout.stock_root / fx.OBB_SHA / next(iter(fx.DATA))).write_bytes(b"short")
        with self.assertRaises(qsb.Refused) as caught:
            self.collect()
        self.assertEqual(caught.exception.code, "pin_size_mismatch")

    def test_a_file_the_pins_do_not_list_is_refused(self):
        fx.write(self.layout.stock_root / fx.OBB_SHA / "_data" / "stray.bin", b"stray")
        with self.assertRaises(qsb.Refused) as caught:
            self.collect()
        self.assertEqual(caught.exception.code, "pin_unlisted_file")

    def test_a_pinned_file_the_tree_lacks_is_refused(self):
        (self.layout.stock_root / fx.OBB_SHA / next(iter(fx.DATA))).unlink()
        with self.assertRaises(qsb.Refused) as caught:
            self.collect()
        self.assertEqual(caught.exception.code, "pin_missing_file")

    def test_no_repacked_apk_is_refused(self):
        self.layout.repacked_apk.unlink()
        with self.assertRaises(qsb.Refused) as caught:
            self.collect()
        self.assertEqual(caught.exception.code, "repack_missing")

    def test_a_missing_stock_data_directory_names_the_step_to_run(self):
        import shutil
        shutil.rmtree(self.layout.stock_root)
        with self.assertRaises(qsb.Refused) as caught:
            self.collect()
        self.assertEqual(caught.exception.code, "stock_data_missing")
        self.assertIn("just quest-stock-data", caught.exception.detail)

    def test_a_second_collect_replaces_the_first(self):
        self.collect()
        fx.write(self.layout.repacked_apk, b"another signed apk")
        status, _ = self.collect()
        self.assertEqual(status, 0)
        self.assertEqual((self.layout.out / qsb.SIGNED_APK_NAME).read_bytes(), b"another signed apk")


class CheckTest(StoreBuildCase):
    def test_no_manifest_is_a_miss(self):
        status, lines = self.check()
        self.assertEqual(status, qsb.EXIT_MISS)
        self.assertEqual([l["event"] for l in lines], ["cache_miss"])
        self.assertIn("nothing was built", lines[0]["reason"])

    def test_the_same_inputs_are_a_hit_that_changes_nothing(self):
        self.collect()
        before = {p: (p.stat().st_mtime_ns, p.stat().st_ino) for p in self.layout.out.rglob("*") if p.is_file()}
        status, lines = self.check()
        self.assertEqual(status, 0)
        self.assertEqual([l["event"] for l in lines], ["cache_hit"])
        self.assertEqual(lines[0]["data_files"], len(fx.DATA))
        after = {p: (p.stat().st_mtime_ns, p.stat().st_ino) for p in self.layout.out.rglob("*") if p.is_file()}
        self.assertEqual(before, after)

    def test_each_input_that_changes_makes_a_miss_naming_it(self):
        def change_apk(s): s.apk.write_bytes(b"another store apk")
        def change_shim(s): fx.write(s.layout.shim, b"sentinel shim v2")
        def change_info(s): fx.write(s.layout.build_info, b"another build info")
        def change_patcher(s): fx.write(s.layout.patcher, b"patcher v2")
        def change_stock(s): fx.write(s.layout.stock_root / fx.OBB_SHA / "stock-data.json", b'{"schema": 2}\n')
        def change_tool(s): fx.write(s.layout.root / "tools" / "quest_repack_libs.py", b"repack libs v2")
        cases = {
            "store_apk": (change_apk, {}),
            "sentinel_library": (change_shim, {}),
            "build_info": (change_info, {}),
            "libr15_patcher": (change_patcher, {}),
            "stock_data": (change_stock, {}),
            "repack_tools": (change_tool, {}),
            "repack_recipe": (lambda s: None, {"recipe": fx.RECIPE + "    echo changed\n"}),
            "signing_key": (lambda s: None, {"signer": "cd" * 32}),
        }
        for name, (change, kwargs) in cases.items():
            with self.subTest(name):
                tmp = tempfile.TemporaryDirectory()
                self.addCleanup(tmp.cleanup)
                case = StoreBuildCase()
                case.layout, case.apk = fx.make_root(pathlib.Path(tmp.name))
                run_logged(qsb.collect, case.layout, case.apk, fx.RECIPE, fx.SIGNER)
                change(case)
                status, lines = run_logged(qsb.check, case.layout, case.apk, kwargs.get("recipe", fx.RECIPE), kwargs.get("signer", fx.SIGNER))
                self.assertEqual(status, qsb.EXIT_MISS)
                miss = [l for l in lines if l["event"] == "cache_miss"][0]
                self.assertEqual(miss["reason"], "inputs changed: " + name)
                self.assertFalse(case.layout.manifest.exists())  # a miss leaves no manifest to be trusted
                self.assertIn("manifest_removed", [l["event"] for l in lines])

    def test_an_output_that_changed_is_a_miss(self):
        data_file = next(iter(fx.DATA))
        def tamper_data(s):
            (s.layout.out / data_file).unlink()  # the files are links to the cache: break the link, then change the copy
            (s.layout.out / data_file).write_bytes(b"x" * len(fx.DATA[data_file]))
        def tamper_apk(s):
            (s.layout.out / qsb.SIGNED_APK_NAME).unlink()
            (s.layout.out / qsb.SIGNED_APK_NAME).write_bytes(b"another apk")
        def delete_data(s): (s.layout.out / data_file).unlink()
        def add_data(s): fx.write(s.layout.out / "_data" / "stray.bin", b"stray")
        def delete_apk(s): (s.layout.out / qsb.SIGNED_APK_NAME).unlink()
        for name, change in (("tamper_data", tamper_data), ("tamper_apk", tamper_apk), ("delete_data", delete_data), ("add_data", add_data), ("delete_apk", delete_apk)):
            with self.subTest(name):
                tmp = tempfile.TemporaryDirectory()
                self.addCleanup(tmp.cleanup)
                case = StoreBuildCase()
                case.layout, case.apk = fx.make_root(pathlib.Path(tmp.name))
                run_logged(qsb.collect, case.layout, case.apk, fx.RECIPE, fx.SIGNER)
                change(case)
                status, lines = run_logged(qsb.check, case.layout, case.apk, fx.RECIPE, fx.SIGNER)
                self.assertEqual(status, qsb.EXIT_MISS)
                self.assertNotIn("inputs changed", [l for l in lines if l["event"] == "cache_miss"][0]["reason"])

    def test_a_manifest_that_does_not_parse_is_a_miss(self):
        self.collect()
        self.layout.manifest.write_text("{not json")
        status, lines = self.check()
        self.assertEqual(status, qsb.EXIT_MISS)
        self.assertIn("cannot be used", [l for l in lines if l["event"] == "cache_miss"][0]["reason"])

    def test_the_key_is_the_same_for_the_same_inputs(self):
        _, first = self.collect()
        _, second = self.check()
        self.assertEqual(first[-1]["key"], second[0]["key"])
        self.assertRegex(first[-1]["key"], r"^[0-9a-f]{64}$")

    def test_a_missing_build_output_is_refused_naming_it(self):
        self.layout.shim.unlink()
        with self.assertRaises(qsb.Refused) as caught:
            self.check()
        self.assertEqual(caught.exception.code, "build_output_missing")
        self.assertIn("sentinel library", caught.exception.detail)


class PinParserTest(unittest.TestCase):
    def test_the_pin_file_of_the_tree_parses_and_agrees_with_the_cpp_test_values(self):
        obb_sha, obb_size, files = qsb.parse_pins((REPO / "tools" / "quest_stock_data.pin").read_text())
        self.assertEqual(obb_sha, "8020d1791c8806b3c9592806d2476c9b9c9771cbfcbce112914b357b66180607")
        self.assertEqual(obb_size, 946094131)
        # The values src/quest/tests/obb_pinned_test.cpp writes out from the measurements in #2081.
        cpp = (REPO / "src" / "quest" / "tests" / "obb_pinned_test.cpp").read_text()
        for entry in files:
            self.assertIn(f'"{entry["path"]}", "{entry["sha256"]}", {entry["bytes"]}', cpp)
        self.assertEqual(len(files), 4)

    def test_a_bad_pin_file_is_refused(self):
        for text in ("", "obb_sha256 zz\n", "bogus 1\n", f"obb_sha256 {fx.OBB_SHA}\nobb_size 5\n"):
            with self.subTest(text), self.assertRaises(qsb.Refused):
                qsb.parse_pins(text)


if __name__ == "__main__":
    unittest.main()
