"""#1857: tools/echomod/plugins_override_generator.py makes the PLUGINS-card override set from the player's own install.

No byte of the game's own data is in the repository (RELEASING.md "Game-derived packages"), so these tests build a
synthetic stock resource and check the generator's rules on it: the stock is checked before the edits and the result after,
a copy never reaches outside the stock, nothing is written unless every file builds, and the install is never written. The
repository's edit list (plugins_override_patches.json) is checked for shape and for carrying no game data. Two checks need
the game and skip, saying so, without it:

  * NEVR_GAME_DATA_DIR names the game's `_data/<id>` folder: the real stock resources must match the edit list's recorded
    hashes, and the generated files must match the recorded hashes.
  * NEVR_PLUGINS_CARD_OVERRIDE_DIR also names a finished override set: the generated files must equal it byte for byte.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import random
import shutil
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
ROOT = TESTS_DIR.parent.parent
ECHOMOD = ROOT / "tools" / "echomod"
sys.path.insert(0, str(ECHOMOD))
import plugins_override_generator as gen  # noqa: E402
from radhash import rad_hash  # noqa: E402

DATA_DIR_ENV = gen.DATA_DIR_ENV
REFERENCE_ENV = "NEVR_PLUGINS_CARD_OVERRIDE_DIR"
LEVEL = "0xac360e41e4ede056"
FILE_COUNT = 13
# A put longer than this, or more of them in one file than the second bound, is game data pasted into the table.
MAX_PUT_BYTES = 128
MAX_PUT_BYTES_PER_FILE = 256
HAVE_ZSTD = importlib.util.find_spec("zstandard") is not None


def synthetic_stock(size: int = 400, seed: int = 7) -> bytes:
    return random.Random(seed).randbytes(size)


def entry_for(stock: bytes, ops: list, name: str = "0x1111111111111111") -> dict:
    result = gen.apply_ops(stock, ops)
    return {
        "override_type": name, "override_class": "CFakeCR", "stock_type": "0x2222222222222222", "stock_class": "CFakeCRWin10",
        "stock_size": len(stock), "stock_sha256": gen.sha256(stock), "size": len(result), "sha256": gen.sha256(result), "ops": ops,
    }


def table_for(entries: list) -> dict:
    return {"schema": 1, "level": LEVEL, "files": entries}


class ApplyOpsTest(unittest.TestCase):
    def test_copies_and_puts_are_joined_in_order(self):
        stock = bytes(range(32))
        self.assertEqual(gen.apply_ops(stock, [["c", 4, 3], ["p", "ff00"], ["c", 0, 2]]), bytes([4, 5, 6, 0xFF, 0, 0, 1]))

    def test_a_copy_past_the_end_of_the_stock_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(10), [["c", 8, 3]])

    def test_a_copy_that_ends_exactly_at_the_end_is_allowed(self):
        self.assertEqual(gen.apply_ops(bytes(range(10)), [["c", 8, 2]]), bytes([8, 9]))

    def test_a_copy_of_nothing_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(10), [["c", 2, 0]])

    def test_a_negative_offset_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(10), [["c", -1, 2]])

    def test_an_unknown_edit_kind_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(10), [["x", 1, 2]])

    def test_a_put_that_is_not_hex_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(10), [["p", "zz"]])

    def test_a_copy_with_the_wrong_number_of_fields_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(10), [["c", 1]])


class LoadTableTest(unittest.TestCase):
    def test_a_table_of_another_schema_is_refused(self):
        tmp = Path(tempfile.mkdtemp(prefix="plugins-override-table-"))
        self.addCleanup(shutil.rmtree, tmp, True)
        path = tmp / "patches.json"
        path.write_text(json.dumps({"schema": 2, "level": LEVEL, "files": []}))
        with self.assertRaises(gen.PatchTableError):
            gen.load_table(path)


class BuildFileTest(unittest.TestCase):
    def setUp(self):
        self.stock = synthetic_stock()
        self.ops = [["c", 0, 100], ["p", "1122334455667788"], ["c", 100, 300]]
        self.entry = entry_for(self.stock, self.ops)

    def test_the_file_is_the_stock_with_the_edits(self):
        built = gen.build_file(self.entry, self.stock)
        self.assertEqual(built, self.stock[:100] + bytes.fromhex("1122334455667788") + self.stock[100:])

    def test_a_stock_that_differs_by_one_byte_is_refused_by_name(self):
        other = bytearray(self.stock)
        other[250] ^= 1
        with self.assertRaises(gen.StockMismatch) as caught:
            gen.build_file(self.entry, bytes(other))
        self.assertIn("StockMismatch", str(caught.exception))
        self.assertIn(gen.sha256(bytes(other)), str(caught.exception))

    def test_a_stock_of_another_size_is_refused(self):
        with self.assertRaises(gen.StockMismatch):
            gen.build_file(self.entry, self.stock + b"\0")

    def test_a_recorded_hash_that_the_edits_do_not_give_is_refused(self):
        self.entry["sha256"] = "0" * 64
        with self.assertRaises(gen.OutputMismatch):
            gen.build_file(self.entry, self.stock)

    def test_a_recorded_size_that_the_edits_do_not_give_is_refused(self):
        self.entry["size"] += 1
        with self.assertRaises(gen.OutputMismatch):
            gen.build_file(self.entry, self.stock)


class DeriveOpsTest(unittest.TestCase):
    def roundtrip(self, stock: bytes, target: bytes) -> list:
        ops = gen.derive_ops(stock, target)
        self.assertEqual(gen.apply_ops(stock, ops), target)
        return ops

    def test_an_in_place_edit_is_a_put_between_two_copies(self):
        stock = synthetic_stock(200)
        target = stock[:50] + b"\xde\xad\xbe\xef\x01\x02\x03\x04\x05" + stock[59:]
        ops = self.roundtrip(stock, target)
        self.assertEqual([op[0] for op in ops], ["c", "p", "c"])

    def test_an_inserted_clone_of_a_stock_range_is_a_copy_not_bytes(self):
        stock = synthetic_stock(300)
        target = stock + stock[40:120]
        ops = self.roundtrip(stock, target)
        self.assertEqual(sum(len(bytes.fromhex(op[1])) for op in ops if op[0] == "p"), 0)

    def test_bytes_of_our_own_are_puts(self):
        stock = synthetic_stock(100)
        target = stock + b"\xaa\xbb\xcc"
        ops = self.roundtrip(stock, target)
        self.assertEqual(ops[-1], ["p", "aabbcc"])

    def test_of_equal_runs_the_one_nearest_the_last_copy_is_taken(self):
        rng = random.Random(3)
        run, middle, tail = rng.randbytes(30), rng.randbytes(30), rng.randbytes(30)
        stock = run + middle + run + tail  # the run is in stock twice, at 0 and at 60
        ops = self.roundtrip(stock, run + middle + b"" + run)
        self.assertEqual(ops, [["c", 0, 60], ["p", "0102"], ["c", 60, 30]])

    def test_an_identical_file_is_one_copy(self):
        stock = synthetic_stock(120)
        self.assertEqual(self.roundtrip(stock, stock), [["c", 0, 120]])

    def test_a_shorter_target_with_a_range_removed(self):
        stock = synthetic_stock(300)
        self.roundtrip(stock, stock[:80] + stock[200:])


class GenerateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="plugins-override-gen-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.stock_a = synthetic_stock(300, 1)
        self.stock_b = synthetic_stock(200, 2)
        self.table = table_for([
            entry_for(self.stock_a, [["c", 0, 300], ["p", "01"]], "0x1111111111111111"),
            entry_for(self.stock_b, [["p", "02"], ["c", 0, 200]], "0x3333333333333333"),
        ])
        self.stocks = {"0x1111111111111111": self.stock_a, "0x3333333333333333": self.stock_b}

    def test_each_file_is_written_under_its_override_name(self):
        out = self.tmp / "deeper" / "out"  # the folder does not exist yet
        paths = gen.generate(self.table, self.stocks, out)
        self.assertEqual(sorted(p.name for p in paths), [f"0x1111111111111111.{LEVEL}", f"0x3333333333333333.{LEVEL}"])
        self.assertEqual((out / f"0x1111111111111111.{LEVEL}").read_bytes(), self.stock_a + b"\x01")
        self.assertEqual((out / f"0x3333333333333333.{LEVEL}").read_bytes(), b"\x02" + self.stock_b)

    def test_nothing_is_written_when_one_stock_is_wrong(self):
        out = self.tmp / "out"
        self.stocks["0x3333333333333333"] = synthetic_stock(200, 99)
        with self.assertRaises(gen.StockMismatch):
            gen.generate(self.table, self.stocks, out)
        self.assertFalse(out.exists() and any(out.iterdir()))

    def test_a_missing_stock_is_refused(self):
        del self.stocks["0x3333333333333333"]
        with self.assertRaises(gen.StockMissing):
            gen.generate(self.table, self.stocks, self.tmp / "out")

    def test_existing_files_are_not_replaced_without_force(self):
        out = self.tmp / "out"
        gen.generate(self.table, self.stocks, out)
        target = out / f"0x1111111111111111.{LEVEL}"
        target.write_bytes(b"mine")
        with self.assertRaises(gen.UsageError):
            gen.generate(self.table, self.stocks, out)
        self.assertEqual(target.read_bytes(), b"mine")
        gen.generate(self.table, self.stocks, out, force=True)
        self.assertEqual(target.read_bytes(), self.stock_a + b"\x01")

    def test_an_output_folder_inside_the_install_is_refused(self):
        data = self.tmp / "install" / "_data" / "123"
        data.mkdir(parents=True)
        for out in (data, data / "overrides"):
            with self.assertRaises(gen.UsageError):
                gen.generate(self.table, self.stocks, out, data_dir=data)
        self.assertEqual(list(data.iterdir()), [])

    def test_a_folder_beside_the_install_is_allowed(self):
        data = self.tmp / "install" / "_data" / "123"
        data.mkdir(parents=True)
        gen.generate(self.table, self.stocks, self.tmp / "install" / "_data" / "123-out", data_dir=data)


class StockForTest(unittest.TestCase):
    def test_each_override_type_gets_the_stock_resource_of_its_own_stock_type(self):
        one, two = synthetic_stock(50, 11), synthetic_stock(60, 12)
        a = entry_for(one, [["c", 0, 50]], "0x1111111111111111")
        b = {**entry_for(two, [["c", 0, 60]], "0x3333333333333333"), "stock_type": "0x4444444444444444"}
        seen = {}

        def fake_read_stock(data_dir, level_hash, type_hashes):
            seen["call"] = (data_dir, level_hash, list(type_hashes))
            return {0x2222222222222222: one, 0x4444444444444444: two}

        with unittest.mock.patch.object(gen, "read_stock", fake_read_stock):
            got = gen.stock_for(table_for([a, b]), Path("somewhere"))
        self.assertEqual(got, {"0x1111111111111111": one, "0x3333333333333333": two})
        self.assertEqual(seen["call"], (Path("somewhere"), int(LEVEL, 16), [0x2222222222222222, 0x4444444444444444]))


class CommandLineTest(unittest.TestCase):
    def run_main(self, argv: list[str], env: dict | None = None) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        saved = os.environ.pop(DATA_DIR_ENV, None)
        if env:
            os.environ.update(env)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = gen.main(argv)
        finally:
            os.environ.pop(DATA_DIR_ENV, None)
            if saved is not None:
                os.environ[DATA_DIR_ENV] = saved
        return code, out.getvalue(), err.getvalue()

    def test_no_data_directory_is_refused_and_the_message_names_both_ways_to_give_it(self):
        code, _, err = self.run_main(["generate", "--out", "unused"])
        self.assertEqual(code, gen.EXIT_REFUSED)
        self.assertIn("UsageError", err)
        self.assertIn("--data-dir", err)
        self.assertIn(DATA_DIR_ENV, err)

    def test_a_data_directory_that_is_not_a_directory_is_refused(self):
        code, _, err = self.run_main(["generate", "--data-dir", str(TESTS_DIR / "no-such-folder"), "--out", "unused"])
        self.assertEqual(code, gen.EXIT_REFUSED)
        self.assertIn("is not a directory", err)

    def test_the_environment_variable_stands_in_for_the_flag(self):
        code, _, err = self.run_main(["generate", "--out", "unused"], env={DATA_DIR_ENV: str(TESTS_DIR / "no-such-folder")})
        self.assertEqual(code, gen.EXIT_REFUSED)
        self.assertIn(str(TESTS_DIR / "no-such-folder"), err)


@unittest.skipUnless(HAVE_ZSTD, "zstandard is not installed: the archive reader (rad_archive_tool) cannot run, so the read of a real archive is not tested")
class ReadStockTest(unittest.TestCase):
    """read_stock and the whole command, against a small archive the test packs with rad_archive_tool itself."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="plugins-override-arch-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.level = int(LEVEL, 16)
        self.stock = synthetic_stock(300, 5)
        src = self.tmp / "in" / "abcd1234"
        (src / "0x2222222222222222").mkdir(parents=True)
        (src / "0x2222222222222222" / f"{LEVEL}.bin").write_bytes(self.stock)
        (src / "0x2222222222222222" / "0x5.bin").write_bytes(b"another level's resource")
        (src / "0x4444444444444444").mkdir()
        (src / "0x4444444444444444" / f"{LEVEL}.bin").write_bytes(b"a type nobody asked for")
        with contextlib.redirect_stdout(io.StringIO()):
            sys.path.insert(0, str(ECHOMOD))
            import rad_archive_tool as rad
            rad.repack_archive(self.tmp / "in", self.tmp / "game")
        self.data_dir = self.tmp / "game"

    def test_the_stock_resource_of_the_level_and_type_is_read(self):
        got = gen.read_stock(self.data_dir, self.level, [0x2222222222222222])
        self.assertEqual(got, {0x2222222222222222: self.stock})

    def test_a_type_the_archive_does_not_have_is_refused(self):
        with self.assertRaises(gen.StockMissing):
            gen.read_stock(self.data_dir, self.level, [0x9999999999999999])

    def test_a_type_found_twice_is_refused(self):
        """Two manifests that both hold the level's resource of the type: which one is the game's is not known."""
        twin = self.tmp / "in2" / "efgh5678" / "0x2222222222222222"
        twin.mkdir(parents=True)
        (twin / f"{LEVEL}.bin").write_bytes(b"a second copy")
        import rad_archive_tool as rad
        with contextlib.redirect_stdout(io.StringIO()):
            rad.repack_archive(self.tmp / "in2", self.tmp / "game")
        with self.assertRaises(gen.StockMissing) as caught:
            gen.read_stock(self.data_dir, self.level, [0x2222222222222222])
        self.assertIn("2 resources", str(caught.exception))

    def test_a_folder_with_no_archive_is_refused(self):
        empty = self.tmp / "empty"
        empty.mkdir()
        with self.assertRaises(gen.StockMissing):
            gen.read_stock(empty, self.level, [0x2222222222222222])

    def test_the_command_writes_the_file_from_the_archive(self):
        ops = [["c", 0, 10], ["p", "a1a2"], ["c", 10, 290]]
        entry = entry_for(self.stock, ops, "0x1111111111111111")
        patches = self.tmp / "patches.json"
        patches.write_text(json.dumps(table_for([entry])))
        out = self.tmp / "out"
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink):
            code = gen.main(["generate", "--data-dir", str(self.data_dir), "--patches", str(patches), "--out", str(out)])
        self.assertEqual(code, 0)
        self.assertEqual((out / f"0x1111111111111111.{LEVEL}").read_bytes(), self.stock[:10] + b"\xa1\xa2" + self.stock[10:])
        self.assertIn("wrote 1 files", sink.getvalue())

    def test_the_command_refuses_an_archive_of_another_game_version(self):
        entry = entry_for(synthetic_stock(300, 6), [["c", 0, 300]], "0x1111111111111111")
        patches = self.tmp / "patches.json"
        patches.write_text(json.dumps(table_for([entry])))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = gen.main(["generate", "--data-dir", str(self.data_dir), "--patches", str(patches), "--out", str(self.tmp / "out")])
        self.assertEqual(code, gen.EXIT_REFUSED)
        self.assertIn("StockMismatch", err.getvalue())
        self.assertFalse((self.tmp / "out").exists())


class RepositoryTableTest(unittest.TestCase):
    """The edit list in the repository: its shape, and that it carries no game data."""

    @classmethod
    def setUpClass(cls):
        cls.table = gen.load_table()

    def test_the_level_is_mnu_master(self):
        self.assertEqual(self.table["level"], LEVEL)
        self.assertEqual(rad_hash("mnu_master"), int(LEVEL, 16))

    def test_there_are_thirteen_files_of_thirteen_types(self):
        types = [e["override_type"] for e in self.table["files"]]
        self.assertEqual(len(types), FILE_COUNT)
        self.assertEqual(len(set(types)), FILE_COUNT)

    def test_each_type_hash_is_the_hash_of_its_class_name(self):
        for e in self.table["files"]:
            self.assertEqual(int(e["override_type"], 16), rad_hash(e["override_class"]), e["override_class"])
            self.assertEqual(int(e["stock_type"], 16), rad_hash(e["stock_class"]), e["stock_class"])

    def test_the_edits_add_up_to_the_recorded_size_without_the_game(self):
        for e in self.table["files"]:
            total = sum(op[2] if op[0] == "c" else len(bytes.fromhex(op[1])) for op in e["ops"])
            self.assertEqual(total, e["size"], e["override_class"])

    def test_every_copy_lies_inside_the_recorded_stock_size(self):
        for e in self.table["files"]:
            for op in e["ops"]:
                if op[0] == "c":
                    self.assertTrue(0 <= op[1] and op[2] > 0 and op[1] + op[2] <= e["stock_size"], (e["override_class"], op))

    def test_no_game_data_is_in_the_table(self):
        for e in self.table["files"]:
            puts = [bytes.fromhex(op[1]) for op in e["ops"] if op[0] == "p"]
            self.assertLessEqual(max(len(p) for p in puts), MAX_PUT_BYTES, e["override_class"])
            self.assertLessEqual(sum(len(p) for p in puts), MAX_PUT_BYTES_PER_FILE, e["override_class"])

    def test_the_hashes_are_sha256_hex(self):
        for e in self.table["files"]:
            for key in ("stock_sha256", "sha256"):
                self.assertRegex(e[key], r"^[0-9a-f]{64}$", (e["override_class"], key))

    def test_the_file_is_written_the_way_the_tool_writes_it(self):
        text = (ECHOMOD / "plugins_override_patches.json").read_text()
        self.assertEqual(gen.dump_table(json.loads(text)), text)

    def test_the_component_id_override_names_the_plugins_card(self):
        """The one edit that tells the cards apart: the PLUGINS card's component id, 8 bytes of the page-element resource."""
        e = next(f for f in self.table["files"] if f["override_class"] == "CR15UIPage2ElementCR")
        self.assertEqual(e["override_type"], "0xa691ec467180d00c")
        self.assertIn(["p", "f1bbab161386ec35"], e["ops"])  # 0x35ec861316abbbf1, little-endian


@unittest.skipUnless(os.environ.get(DATA_DIR_ENV), f"{DATA_DIR_ENV} is not set: the real install was not read (not run in CI)")
class RealInstallTest(unittest.TestCase):
    def setUp(self):
        self.data_dir = Path(os.environ[DATA_DIR_ENV])
        self.table = gen.load_table()
        self.tmp = Path(tempfile.mkdtemp(prefix="plugins-override-real-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_the_real_stock_resources_are_the_ones_the_table_was_made_from(self):
        for override_type, stock in gen.stock_for(self.table, self.data_dir).items():
            e = next(f for f in self.table["files"] if f["override_type"] == override_type)
            self.assertEqual((len(stock), gen.sha256(stock)), (e["stock_size"], e["stock_sha256"]), e["stock_class"])

    def test_the_generated_files_are_the_recorded_ones(self):
        paths = gen.generate(self.table, gen.stock_for(self.table, self.data_dir), self.tmp / "out", self.data_dir)
        self.assertEqual(len(paths), FILE_COUNT)
        recorded = {gen.file_name(e, LEVEL): e["sha256"] for e in self.table["files"]}
        self.assertEqual({p.name: gen.sha256(p.read_bytes()) for p in paths}, recorded)

    def test_the_generated_files_equal_the_finished_override_set(self):
        reference = os.environ.get(REFERENCE_ENV)
        if not reference:
            self.skipTest(f"{REFERENCE_ENV} is not set: the generated files were not compared with a finished override set")
        paths = gen.generate(self.table, gen.stock_for(self.table, self.data_dir), self.tmp / "out", self.data_dir)
        for path in paths:
            self.assertEqual(path.read_bytes(), (Path(reference) / path.name).read_bytes(), path.name)


if __name__ == "__main__":
    unittest.main()
