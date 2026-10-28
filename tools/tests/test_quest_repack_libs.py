"""The library entries of the Quest repack (#1117): a plugin may not take a game library's place, and every library of the
store APK survives signing.

Two layers. The unit tests call tools/quest_repack_libs.py on small zip files made here. The end-to-end tests run
`just android-repack-apk` for real (the fixture of test_quest_repack_version.py: a fixture APK, a compiled shim, a
fixture key through the real tools/quest_keystore.sh with a fake `pass`) with an AArch64 plugin made by clang, and read the
SIGNED APK back with the standard zip reader.
"""
from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "tests"))
import quest_repack_libs as libs  # noqa: E402
import work_root  # noqa: E402
from quest_patcher_standin import MARKER, make_standin, recorded_argv  # noqa: E402
from test_quest_repack_version import DEV_VERSION, JUSTFILE, MISSING, RepackFixture  # noqa: E402

LIBR15 = b"\x7fELF the game\0" * 50  # what RepackFixture.make_inputs puts under lib/arm64-v8a/libr15.so
PATCHED_LIBR15 = LIBR15 + MARKER  # what the stand-in patcher (quest_patcher_standin.py) makes of it
AARCH64_MISSING = [t for t in ("clang", "ld.lld", "llvm-nm", "llvm-readelf") if shutil.which(t) is None]


def make_zip(path: pathlib.Path, entries: dict[str, bytes]) -> pathlib.Path:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return path


class PluginNameTest(unittest.TestCase):
    """What a plugin's file name collides with."""

    STORE = ["lib/arm64-v8a/libr15.so", "lib/arm64-v8a/libovrplatformloader.so", "lib/armeabi-v7a/libother.so"]

    def test_a_name_no_entry_has_collides_with_nothing(self):
        self.assertEqual(libs.plugin_name_collisions(self.STORE, "libgood.so"), [])

    def test_a_store_library_name_collides(self):
        self.assertEqual(libs.plugin_name_collisions(self.STORE, "libr15.so"), ["lib/arm64-v8a/libr15.so"])

    def test_the_name_collides_in_any_abi_directory(self):
        self.assertEqual(libs.plugin_name_collisions(self.STORE, "libother.so"), ["lib/armeabi-v7a/libother.so"])

    def test_a_library_the_recipe_stages_collides_even_when_the_store_lacks_it(self):
        found = libs.plugin_name_collisions(self.STORE, "libovrplatformloader_orig.so")
        self.assertEqual(len(found), 1)
        self.assertIn("staged by the recipe", found[0])

    def test_the_loader_name_collides_with_the_store_entry_and_the_staged_shim(self):
        self.assertEqual(len(libs.plugin_name_collisions(self.STORE, "libovrplatformloader.so")), 2)

    def test_only_a_whole_file_name_collides(self):
        self.assertEqual(libs.plugin_name_collisions(self.STORE, "libr15.so.1"), [])
        self.assertEqual(libs.plugin_name_collisions(self.STORE, "r15.so"), [])
        self.assertEqual(libs.plugin_name_collisions(["assets/libr15.so"], "libr15.so"), ["assets/libr15.so"])


class CheckPluginNameFromFileTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-repack-libs-", dir=work_root.test_scratch())
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)

    def test_the_store_apk_is_read_not_assumed(self):
        apk = make_zip(self.tmp / "store.apk", {"lib/arm64-v8a/libr15.so": b"x", "lib/arm64-v8a/libextra.so": b"y",
                                                "assets/a.bin": b"z"})
        self.assertEqual(libs.check_plugin_name(apk, self.tmp / "libextra.so"), ["lib/arm64-v8a/libextra.so"])
        self.assertEqual(libs.check_plugin_name(apk, self.tmp / "libfree.so"), [])
        self.assertEqual(libs.check_plugin_name(apk, self.tmp / "a.bin"), [])  # only lib/ entries count

    def test_the_command_line_exits_one_and_names_the_entry(self):
        apk = make_zip(self.tmp / "store.apk", {"lib/arm64-v8a/libr15.so": b"x"})
        plugin = self.tmp / "libr15.so"
        plugin.write_bytes(b"p")
        refused = subprocess.run([sys.executable, "-I", str(REPO / "tools" / "quest_repack_libs.py"), "check-plugin-name",
                                  "--apk", str(apk), "--plugin", str(plugin)], capture_output=True, text=True)
        self.assertEqual(refused.returncode, 1)
        self.assertIn("plugin libr15.so would replace lib/arm64-v8a/libr15.so", refused.stderr)
        self.assertNotIn("OK", refused.stdout)
        plugin_ok = self.tmp / "libok.so"
        plugin_ok.write_bytes(b"p")
        accepted = subprocess.run([sys.executable, "-I", str(REPO / "tools" / "quest_repack_libs.py"), "check-plugin-name",
                                   "--apk", str(apk), "--plugin", str(plugin_ok)], capture_output=True, text=True)
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertIn("check-plugin-name OK", accepted.stdout)


class VerifySignedTest(unittest.TestCase):
    """The signed APK against the store APK, the staged files and the patcher's output (ADR 0021 section 9, test 7).

    The patcher is a stand-in (quest_patcher_standin.py): it makes `store bytes + MARKER` of whatever it is given."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-repack-libs-", dir=work_root.test_scratch())
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.store = {
            "AndroidManifest.xml": b"manifest",
            "lib/arm64-v8a/libovrplatformloader.so": b"store loader",
            "lib/arm64-v8a/libr15.so": b"the game",
            "lib/arm64-v8a/libpnsovr.so": b"pns",
            "lib/armeabi-v7a/libold.so": b"old abi",
        }
        self.staged = {
            "libovrplatformloader.so": b"the shim",
            "libovrplatformloader_orig.so": b"store loader, renamed",
            "libr15.so": b"the game" + MARKER,
            "libplugin.so": b"a plugin",
        }
        self.signed = {
            **{k: v for k, v in self.store.items() if k not in ("lib/arm64-v8a/libovrplatformloader.so", "lib/arm64-v8a/libr15.so")},
            **{f"lib/arm64-v8a/{k}": v for k, v in self.staged.items()},
        }
        self.patcher = make_standin(self.tmp / "bin", "ok")
        self.extra_rows = None

    def run_verify(self) -> list[str]:
        source = make_zip(self.tmp / "store.apk", self.store)
        signed = make_zip(self.tmp / "signed.apk", self.signed)
        stage = self.tmp / "stage"
        shutil.rmtree(stage, ignore_errors=True)
        (stage / "lib" / "arm64-v8a").mkdir(parents=True)
        for name, data in self.staged.items():
            (stage / "lib" / "arm64-v8a" / name).write_bytes(data)
        return libs.verify_signed(source, signed, stage, self.patcher, self.extra_rows, self.tmp)

    def run_cli(self):
        """The same check through the command line (exit code, stdout, stderr)."""
        make_zip(self.tmp / "store.apk", self.store)
        make_zip(self.tmp / "signed.apk", self.signed)
        stage = self.tmp / "stage"
        shutil.rmtree(stage, ignore_errors=True)
        (stage / "lib" / "arm64-v8a").mkdir(parents=True)
        for name, data in self.staged.items():
            (stage / "lib" / "arm64-v8a" / name).write_bytes(data)
        command = [sys.executable, "-I", str(REPO / "tools" / "quest_repack_libs.py"), "verify-signed",
                   "--source", str(self.tmp / "store.apk"), "--signed", str(self.tmp / "signed.apk"), "--stage", str(stage),
                   "--patcher", str(self.patcher), "--scratch", str(self.tmp)]
        if self.extra_rows is not None:
            command += ["--extra-rows", str(self.extra_rows)]
        return subprocess.run(command, capture_output=True, text=True)

    def test_an_apk_where_everything_survived_has_no_problems(self):
        self.assertEqual(self.run_verify(), [])

    def test_the_loader_the_shim_replaces_may_differ(self):
        self.signed["lib/arm64-v8a/libovrplatformloader.so"] = b"the shim"
        self.assertEqual(self.run_verify(), [])

    def test_a_store_library_missing_from_the_signed_apk_is_named(self):
        del self.signed["lib/arm64-v8a/libpnsovr.so"]
        problems = self.run_verify()
        self.assertEqual(len(problems), 1)
        self.assertIn("lib/arm64-v8a/libpnsovr.so", problems[0])
        self.assertIn("missing from the signed APK", problems[0])

    def test_a_store_library_with_other_bytes_is_named(self):
        self.signed["lib/arm64-v8a/libpnsovr.so"] = b"a plugin took this name"
        problems = self.run_verify()
        self.assertEqual(len(problems), 1)
        self.assertIn("lib/arm64-v8a/libpnsovr.so", problems[0])
        self.assertIn("different bytes than the store APK", problems[0])

    def test_a_library_of_another_abi_is_held_to_the_same_rule(self):
        del self.signed["lib/armeabi-v7a/libold.so"]
        self.assertEqual(len(self.run_verify()), 1)

    def test_a_staged_library_missing_from_the_signed_apk_is_named(self):
        del self.signed["lib/arm64-v8a/libplugin.so"]
        problems = self.run_verify()
        self.assertEqual(len(problems), 1)
        self.assertIn("libplugin.so: staged, missing", problems[0])

    def test_a_staged_library_with_other_bytes_is_named(self):
        self.signed["lib/arm64-v8a/libovrplatformloader_orig.so"] = b"something else"
        problems = self.run_verify()
        self.assertEqual(len(problems), 1)
        self.assertIn("different bytes than the staged file", problems[0])

    def test_every_problem_is_reported_not_only_the_first(self):
        del self.signed["lib/arm64-v8a/libpnsovr.so"]
        del self.signed["lib/arm64-v8a/libplugin.so"]
        self.assertEqual(len(self.run_verify()), 2)

    # ---- libr15.so is the patcher's output (ADR 0021 test 7) ------------------------------------------------------------

    def only_problem(self) -> str:
        problems = self.run_verify()
        self.assertEqual(len(problems), 1, problems)
        return problems[0]

    def digest(self, data: bytes) -> str:
        return hashlib.sha256(data).hexdigest()

    def test_the_signed_libr15_is_the_patchers_output_and_the_command_line_says_ok(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("verify-signed OK", result.stdout)

    def test_a_signed_libr15_that_is_the_store_bytes_is_refused(self):
        self.signed["lib/arm64-v8a/libr15.so"] = self.store["lib/arm64-v8a/libr15.so"]
        self.staged["libr15.so"] = self.store["lib/arm64-v8a/libr15.so"]
        problem = self.only_problem()
        self.assertIn("lib/arm64-v8a/libr15.so: the signed APK does not hold the patcher's output", problem)
        self.assertIn(self.digest(b"the game"), problem)  # the signed (= store) digest is named
        result = self.run_cli()
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("OK", result.stdout)

    def test_a_signed_libr15_made_by_the_patcher_from_another_input_is_refused(self):
        other = b"another libr15" + MARKER  # what the patcher makes of a different store file
        self.signed["lib/arm64-v8a/libr15.so"] = other
        self.staged["libr15.so"] = other
        problem = self.only_problem()
        self.assertIn("does not hold the patcher's output", problem)
        self.assertIn(self.digest(other), problem)
        self.assertIn(self.digest(b"the game" + MARKER), problem)

    def test_a_patcher_that_exits_non_zero_is_refused_with_its_message(self):
        self.patcher = make_standin(self.tmp / "bin-fail", "fail")
        problem = self.only_problem()
        self.assertIn("exited 3", problem)
        self.assertIn("standin patcher: refused", problem)
        self.assertEqual(self.run_cli().returncode, 1)

    def test_a_patcher_that_writes_no_file_is_refused(self):
        self.patcher = make_standin(self.tmp / "bin-nowrite", "nowrite")
        self.assertIn("exited 0 and wrote no file", self.only_problem())

    def test_a_patcher_that_writes_an_empty_file_is_refused(self):
        self.patcher = make_standin(self.tmp / "bin-empty", "empty")
        self.assertIn("exited 0 and wrote no file", self.only_problem())

    def test_a_patcher_that_is_not_there_is_refused(self):
        self.patcher = self.tmp / "no-such-patcher"
        self.assertIn("did not run to its end", self.only_problem())
        self.assertEqual(self.run_cli().returncode, 1)

    def test_a_patcher_whose_output_is_the_store_file_is_refused(self):
        self.patcher = make_standin(self.tmp / "bin-same", "same")
        self.signed["lib/arm64-v8a/libr15.so"] = b"the game"
        self.staged["libr15.so"] = b"the game"
        self.assertIn("nothing was patched", self.only_problem())

    def test_a_libr15_missing_from_the_signed_apk_is_refused_by_name(self):
        del self.signed["lib/arm64-v8a/libr15.so"]
        del self.staged["libr15.so"]
        problem = self.only_problem()
        self.assertIn("lib/arm64-v8a/libr15.so", problem)
        self.assertIn("missing from the signed APK", problem)
        self.assertEqual(self.run_cli().returncode, 1)

    def test_a_store_apk_without_libr15_is_refused(self):
        del self.store["lib/arm64-v8a/libr15.so"]
        problems = self.run_verify()
        self.assertTrue(any("missing from the store APK" in p for p in problems), problems)

    def test_the_three_digests_are_printed_on_success_and_on_refusal(self):
        for refuse in (False, True):
            if refuse:
                self.signed["lib/arm64-v8a/libr15.so"] = b"the game"
            result = self.run_cli()
            self.assertEqual(result.returncode, 1 if refuse else 0, result.stderr)
            self.assertIn(f"sha256 store   {self.digest(b'the game')}", result.stdout)
            self.assertIn(f"sha256 patched {self.digest(b'the game' + MARKER)}", result.stdout)
            signed = b"the game" if refuse else b"the game" + MARKER
            self.assertIn(f"sha256 signed  {self.digest(signed)}", result.stdout)

    def test_the_patcher_is_given_the_store_file_and_the_extra_rows_file(self):
        rows = self.tmp / "rows.json"
        rows.write_text("{}")
        self.extra_rows = rows
        self.assertEqual(self.run_verify(), [])
        argv = recorded_argv(self.patcher)
        flags = dict(zip(argv[0::2], argv[1::2]))
        self.assertEqual(sorted(flags), ["--extra-rows", "--out", "--report", "--stock"])
        self.assertEqual(flags["--extra-rows"], str(rows))

    def test_without_extra_rows_the_patcher_is_given_none(self):
        self.assertEqual(self.run_verify(), [])
        self.assertNotIn("--extra-rows", recorded_argv(self.patcher))

    def test_the_patcher_runs_on_a_copy_of_the_store_file_in_a_directory_that_is_removed(self):
        before = set(self.tmp.iterdir())
        self.assertEqual(self.run_verify(), [])
        argv = recorded_argv(self.patcher)
        stock = pathlib.Path(dict(zip(argv[0::2], argv[1::2]))["--stock"])
        self.assertEqual(stock.parent.parent, self.tmp)  # under --scratch, not a fixed path
        self.assertFalse(stock.parent.exists())
        new = set(self.tmp.iterdir()) - before
        self.assertEqual({p.name for p in new}, {"store.apk", "signed.apk", "stage"})

    def test_the_signed_libr15_is_compared_with_the_patcher_run_on_this_store_file(self):
        # the digest the signed entry is compared with comes from a run on THIS store file: the stand-in appends the
        # marker to whatever it is given, so a signed libr15 made from a different input never matches
        self.store["lib/arm64-v8a/libr15.so"] = b"a different store libr15"
        problem = self.only_problem()
        self.assertIn("does not hold the patcher's output", problem)


@unittest.skipIf(MISSING or AARCH64_MISSING, f"needs {', '.join(MISSING + AARCH64_MISSING)}")
class RepackWithAPluginTest(RepackFixture):
    """`just android-repack-apk` with plugin=, end to end."""

    def plugin(self, name: str, source: str = "int plugin_marker = 1;\n") -> pathlib.Path:
        out = self.tmp / "plugins"
        out.mkdir(exist_ok=True)
        src = out / (name + ".c")
        src.write_text(source)
        path = out / name
        subprocess.run(["clang", "--target=aarch64-linux-gnu", "-shared", "-fPIC", "-nostdlib", "-fuse-ld=lld",
                        "-o", str(path), str(src)], check=True, capture_output=True)
        return path

    def repack_with(self, plugin: pathlib.Path):
        work = self.tmp / "work"
        result = subprocess.run(
            ["just", "--justfile", str(JUSTFILE), "--working-directory", str(REPO), "android-repack-apk",
             str(self.apk), str(self.shim), os.path.relpath(work, REPO), str(plugin), "", "", str(self.patcher)],
            capture_output=True, text=True, env=self.env, timeout=300)
        self.signed = work / "r15_nevr-sentinel_signed.apk"
        return result

    def add_store_entry(self, name: str, data: bytes) -> None:
        with zipfile.ZipFile(self.apk, "a") as z:
            z.writestr(name, data)

    def test_a_clean_plugin_is_packaged_and_the_game_libraries_survive(self):
        self.make_inputs(DEV_VERSION)
        plugin = self.plugin("libgood.so")
        result = self.repack_with(plugin)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("verify-signed OK", result.stdout)
        with zipfile.ZipFile(self.signed) as z:
            self.assertEqual(z.read("lib/arm64-v8a/libgood.so"), plugin.read_bytes())
            self.assertEqual(z.read("lib/arm64-v8a/libr15.so"), PATCHED_LIBR15)
            self.assertEqual(z.read("lib/arm64-v8a/libovrplatformloader_orig.so")[:4], b"\x7fELF")

    def test_a_plugin_named_like_a_game_library_is_refused_before_anything_is_signed(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libr15.so"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("plugin libr15.so would replace lib/arm64-v8a/libr15.so", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_plugin_named_like_a_library_the_recipe_stages_is_refused(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libovrplatformloader_orig.so"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("staged by the recipe", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_plugin_named_like_a_library_of_another_abi_is_refused(self):
        self.make_inputs(DEV_VERSION)
        self.add_store_entry("lib/armeabi-v7a/libold.so", b"old")
        result = self.repack_with(self.plugin("libold.so"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lib/armeabi-v7a/libold.so", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_plugin_with_a_load_time_initializer_is_refused(self):
        self.make_inputs(DEV_VERSION)
        ctor = "__attribute__((constructor)) static void run(void) {}\nint plugin_marker = 2;\n"
        result = self.repack_with(self.plugin("libctor.so", ctor))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is not an expected initializer", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_repack_without_a_plugin_still_verifies_the_store_libraries(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("verify-signed OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
