"""#1857: tools/echomod/plugins_override_generator.py makes the PLUGINS-card override set from the player's own install.

No byte of the game's own data is in the repository (RELEASING.md "Game-derived packages"). The table the generator reads
(plugins_override_patches.json) holds offsets, lengths, hashes and bytes of our own, and the rule for the bytes of our own is:

  * a put of 8 bytes or more that can carry data (3 or more distinct byte values) is one of the values the table lists by
    name in `ours`, each with a reason; every other put is shorter than 8 bytes or is padding or a count (1 or 2 values);
  * no 8-byte data window of any put occurs in any stock resource of the level. A stock run, a symbol hash included, is a
    copy ("c", "x") or a symbol hash ("h"), never a put.

These tests check the rule on a synthetic stock built here, the committed table against the rule without the game, and, when
the game is there, the table against the real stock. Two groups need the game and skip, saying so, without it:

  * NEVR_GAME_DATA_DIR names the game's `_data/<id>` folder: the real stock resources must match the table's hashes, no put
    may overlap a stock window, and the generated files must match the recorded hashes.
  * NEVR_PLUGINS_CARD_OVERRIDE_DIR also names a finished override set: the generated files must equal it byte for byte, and
    `derive` from it must give the committed table.
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
NEW_ACTOR_HASH = "fec088903e7e7cf6"  # 0xf67c7e3e9088c0fe, little-endian
PLUGINS_COMPONENT_ID = "f1bbab161386ec35"  # 0x35ec861316abbbf1, little-endian
# What the table may list as ours. A new entry is a decision: this list is the review gate for it.
OURS_NAMES = {
    "new_actor_hash", "plugins_component_id", "card_pose_floats", "card_position",
    "card_text_create", "card_text_join", "card_text_join_tip_part", "card_text_plugins_tip",
}
SYMBOL_NAMES = {"tile_create", "tile_join"}
MAX_PUT_BYTES_PER_FILE = 160
HAVE_ZSTD = importlib.util.find_spec("zstandard") is not None


def synthetic_stock(size: int = 400, seed: int = 7) -> bytes:
    return random.Random(seed).randbytes(size)


def table_for(entries: list, ours: list | None = None, symbols: list | None = None) -> dict:
    return {"schema": 2, "level": LEVEL, "symbols": symbols or [], "ours": ours or [], "files": entries}


def entry_for(stock: bytes, ops: list, name: str = "0x1111111111111111", others: dict | None = None) -> dict:
    result = gen.apply_ops(stock, ops, name, others)
    return {
        "override_type": name, "override_class": "CFakeCR", "stock_type": "0x2222222222222222", "stock_class": "CFakeCRWin10",
        "stock_size": len(stock), "stock_sha256": gen.sha256(stock), "size": len(result), "sha256": gen.sha256(result), "ops": ops,
    }


def own(hex_value: str, name: str = "a_value") -> dict:
    return {"name": name, "hex": hex_value, "reason": "ours, for the test"}


class IsDataTest(unittest.TestCase):
    def test_padding_and_counts_are_structure(self):
        self.assertFalse(gen.is_data(bytes(8)))
        self.assertFalse(gen.is_data(bytes.fromhex("0b000000000000000b")))
        self.assertFalse(gen.is_data(b"\xff" * 8))

    def test_three_distinct_values_can_carry_data(self):
        self.assertTrue(gen.is_data(bytes.fromhex("0b0c000000000000")))
        self.assertTrue(gen.is_data(bytes.fromhex(NEW_ACTOR_HASH)))


class ApplyOpsTest(unittest.TestCase):
    def test_copies_and_puts_are_joined_in_order(self):
        stock = bytes(range(32))
        self.assertEqual(gen.apply_ops(stock, [["c", 4, 3], ["p", "ff00"], ["c", 0, 2]]), bytes([4, 5, 6, 0xFF, 0, 0, 1]))

    def test_a_cross_copy_reads_the_other_files_stock(self):
        other = bytes(range(100, 132))
        self.assertEqual(gen.apply_ops(bytes(4), [["x", "0x3", 2, 3]], others={"0x3": other}), bytes([102, 103, 104]))

    def test_a_cross_copy_of_a_file_not_given_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(4), [["x", "0x3", 0, 1]], others={})

    def test_a_cross_copy_past_the_end_of_the_other_stock_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(4), [["x", "0x3", 30, 5]], others={"0x3": bytes(32)})

    def test_a_symbol_hash_is_the_little_endian_hash_of_its_name(self):
        built = gen.apply_ops(bytes(4), [["h", "tile_create"]])
        self.assertEqual(int.from_bytes(built, "little"), rad_hash("tile_create"))
        self.assertEqual(len(built), 8)

    def test_a_symbol_hash_without_a_name_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(4), [["h", ""]])

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
            gen.apply_ops(bytes(10), [["z", 1, 2]])

    def test_a_put_that_is_not_hex_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(10), [["p", "zz"]])

    def test_a_copy_with_the_wrong_number_of_fields_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.apply_ops(bytes(10), [["c", 1]])


class CheckTableTest(unittest.TestCase):
    """The rule for puts, on tables built here."""

    def table(self, ops: list, ours: list | None = None) -> dict:
        stock = synthetic_stock(64)
        entry = {**entry_for(stock, [["c", 0, 64]]), "ops": ops}
        return table_for([entry], ours)

    def test_a_put_shorter_than_a_window_needs_no_listing(self):
        gen.check_table(self.table([["p", "01020304050607"]]))

    def test_a_put_of_a_window_or_more_that_can_carry_data_must_be_listed(self):
        with self.assertRaises(gen.PutNotOurs):
            gen.check_table(self.table([["p", NEW_ACTOR_HASH]]))

    def test_a_listed_put_passes(self):
        gen.check_table(self.table([["p", NEW_ACTOR_HASH]], [own(NEW_ACTOR_HASH)]))

    def test_a_put_that_is_only_a_count_or_padding_needs_no_listing(self):
        gen.check_table(self.table([["p", "0b000000000000000b"], ["p", "00" * 40]]))

    def test_a_listed_value_must_have_a_name_and_a_reason(self):
        for broken in ({"name": "", "hex": NEW_ACTOR_HASH, "reason": "r"}, {"name": "n", "hex": NEW_ACTOR_HASH, "reason": ""}):
            with self.assertRaises(gen.PatchTableError):
                gen.check_table(self.table([["c", 0, 8]], [broken]))

    def test_a_listed_value_shorter_than_a_window_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.check_table(self.table([["c", 0, 8]], [own("0102")]))

    def test_a_value_listed_twice_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.check_table(self.table([["c", 0, 8]], [own(NEW_ACTOR_HASH, "a"), own(NEW_ACTOR_HASH, "b")]))

    def test_a_cross_copy_from_a_file_not_in_the_set_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.check_table(self.table([["x", "0x9999999999999999", 0, 8]]))

    def test_a_cross_copy_outside_the_other_files_recorded_size_is_refused(self):
        stock = synthetic_stock(64)
        a = {**entry_for(stock, [["c", 0, 64]], "0x1111111111111111"), "ops": [["x", "0x3333333333333333", 60, 8]]}
        b = entry_for(stock, [["c", 0, 64]], "0x3333333333333333")
        with self.assertRaises(gen.PatchTableError):
            gen.check_table(table_for([a, b]))

    def test_a_copy_outside_the_recorded_stock_size_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.check_table(self.table([["c", 60, 8]]))

    def test_a_symbol_hash_without_a_name_is_refused(self):
        with self.assertRaises(gen.PatchTableError):
            gen.check_table(self.table([["h", ""]]))


class CheckPutsCleanTest(unittest.TestCase):
    def test_a_put_that_carries_a_stock_window_is_refused_by_name(self):
        stock = synthetic_stock(200)
        with self.assertRaises(gen.PutCarriesStock) as caught:
            gen.check_puts_clean([["p", stock[50:60].hex()]], [stock], "f")
        self.assertIn("PutCarriesStock", str(caught.exception))
        self.assertIn(stock[50:58].hex(), str(caught.exception))

    def test_the_stock_window_may_be_in_any_resource_of_the_level(self):
        mine, theirs = synthetic_stock(100, 1), synthetic_stock(100, 2)
        with self.assertRaises(gen.PutCarriesStock):
            gen.check_puts_clean([["p", theirs[10:18].hex()]], [mine, theirs])

    def test_a_put_of_our_own_passes(self):
        gen.check_puts_clean([["p", NEW_ACTOR_HASH]], [synthetic_stock(200)])

    def test_a_put_shorter_than_a_window_cannot_carry_one(self):
        stock = synthetic_stock(200)
        gen.check_puts_clean([["p", stock[50:57].hex()]], [stock])

    def test_padding_found_in_stock_proves_nothing(self):
        gen.check_puts_clean([["p", "00" * 16]], [bytes(64)])

    def test_the_window_may_straddle_the_end_of_the_put(self):
        stock = synthetic_stock(200)
        with self.assertRaises(gen.PutCarriesStock):
            gen.check_puts_clean([["p", "aa" + stock[50:58].hex()]], [stock])

    def test_copies_and_symbol_hashes_are_not_checked(self):
        stock = synthetic_stock(200)
        gen.check_puts_clean([["c", 0, 100], ["h", "tile_create"]], [stock])


class LoadTableTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="plugins-override-table-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_a_table_of_another_schema_is_refused(self):
        path = self.tmp / "patches.json"
        path.write_text(json.dumps({"schema": 1, "level": LEVEL, "files": []}))
        with self.assertRaises(gen.PatchTableError):
            gen.load_table(path)

    def test_a_table_that_breaks_the_put_rule_is_refused_at_load(self):
        stock = synthetic_stock(64)
        entry = {**entry_for(stock, [["c", 0, 64]]), "ops": [["c", 0, 64], ["p", NEW_ACTOR_HASH]]}
        path = self.tmp / "patches.json"
        path.write_text(json.dumps(table_for([entry])))
        with self.assertRaises(gen.PutNotOurs):
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

    def test_a_recorded_hash_that_the_edits_do_not_give_is_refused(self):
        self.entry["sha256"] = "0" * 64
        with self.assertRaises(gen.OutputMismatch):
            gen.build_file(self.entry, self.stock)

    def test_a_recorded_size_that_the_edits_do_not_give_is_refused(self):
        self.entry["size"] += 1
        with self.assertRaises(gen.OutputMismatch):
            gen.build_file(self.entry, self.stock)

    def test_a_put_that_carries_a_stock_window_stops_the_build(self):
        ops = [["p", self.stock[10:20].hex()]]
        entry = entry_for(self.stock, ops)
        with self.assertRaises(gen.PutCarriesStock):
            gen.build_file(entry, self.stock, resources=[self.stock])

    def test_the_cross_copy_source_is_the_other_files_stock(self):
        other = synthetic_stock(64, 3)
        entry = entry_for(self.stock, [["x", "0x3333333333333333", 8, 16], ["c", 0, 20]], others={"0x3333333333333333": other})
        built = gen.build_file(entry, self.stock, {"0x3333333333333333": other})
        self.assertEqual(built, other[8:24] + self.stock[:20])


class DeriveOpsTest(unittest.TestCase):
    def roundtrip(self, stock: bytes, target: bytes, **kw) -> list:
        ops = gen.derive_ops(stock, target, **kw)
        self.assertEqual(gen.apply_ops(stock, ops, others=kw.get("others")), target)
        return ops

    def test_an_in_place_edit_is_a_put_between_two_copies(self):
        stock = synthetic_stock(200)
        target = stock[:50] + b"\xde\xad\xbe\xef\x01\x02\x03\x04\x05" + stock[59:]
        ops = self.roundtrip(stock, target)
        self.assertEqual([op[0] for op in ops], ["c", "p", "c"])

    def test_an_in_place_edit_keeps_only_the_bytes_that_differ(self):
        stock = bytes(range(1, 200))
        target = bytearray(stock)
        target[60] ^= 0xFF
        target[70] ^= 0xFF
        ops = self.roundtrip(stock, bytes(target))
        self.assertEqual([op for op in ops if op[0] == "p"], [["p", f"{target[60]:02x}"], ["p", f"{target[70]:02x}"]])

    def test_an_edit_at_the_start_of_the_file_is_cut_too(self):
        stock = bytes(range(1, 100))
        ops = self.roundtrip(stock, b"\xee" + stock[1:])
        self.assertEqual(ops[0], ["p", "ee"])

    def test_an_inserted_clone_of_a_stock_range_is_a_copy_not_bytes(self):
        stock = synthetic_stock(300)
        ops = self.roundtrip(stock, stock + stock[40:120])
        self.assertEqual([op for op in ops if op[0] == "p"], [])

    def test_bytes_of_our_own_are_puts(self):
        stock = synthetic_stock(100)
        ops = self.roundtrip(stock, stock + b"\xaa\xbb\xcc")
        self.assertEqual(ops[-1], ["p", "aabbcc"])

    def test_a_value_of_ours_is_kept_whole(self):
        stock = synthetic_stock(120)
        value = bytes.fromhex(NEW_ACTOR_HASH)
        # the stock holds the value's last byte where the value ends: a copy must not take it
        stock2 = stock[:40] + bytes([value[-1]]) + stock[40:]
        target = stock[:40] + value + stock[40:]
        ops = self.roundtrip(stock2, target, ours=[value])
        self.assertIn(["p", NEW_ACTOR_HASH], ops)

    def test_a_symbol_hash_is_written_as_its_name(self):
        stock = synthetic_stock(100)
        target = stock + gen.symbol_bytes("tile_join") + stock[:30]
        ops = self.roundtrip(stock, target, symbols=["tile_join"])
        self.assertIn(["h", "tile_join"], ops)

    def test_a_run_only_another_file_holds_is_a_cross_copy(self):
        stock, other = synthetic_stock(100, 1), synthetic_stock(100, 2)
        target = stock[:30] + other[20:60] + stock[30:]
        ops = self.roundtrip(stock, target, others={"0x3": other})
        self.assertIn(["x", "0x3", 20, 40], ops)

    def test_this_files_stock_is_taken_before_another_files(self):
        stock = synthetic_stock(100, 1)
        ops = self.roundtrip(stock, stock[10:50], others={"0x3": stock})
        self.assertEqual(ops, [["c", 10, 40]])

    def test_this_files_stock_wins_even_when_another_file_matches_longer(self):
        rng = random.Random(5)
        run, more = rng.randbytes(20), rng.randbytes(20)
        stock = rng.randbytes(50) + run + rng.randbytes(50)
        other = run + more
        ops = self.roundtrip(stock, run + more, others={"0x3": other})
        self.assertEqual(ops[0], ["c", 50, 20])

    def test_padding_is_never_copied_from_another_file(self):
        stock = synthetic_stock(100, 1)
        target = stock + bytes(40)
        ops = self.roundtrip(stock, target, others={"0x3": bytes(200)})
        self.assertEqual([op for op in ops if op[0] == "x"], [])

    def test_of_equal_runs_the_one_nearest_the_last_copy_is_taken(self):
        rng = random.Random(3)
        run, middle, tail = rng.randbytes(30), rng.randbytes(30), rng.randbytes(30)
        stock = run + middle + run + tail  # the run is in stock twice, at 0 and at 60
        ops = self.roundtrip(stock, run + middle + b"\x01\x02" + run)
        self.assertEqual(ops, [["c", 0, 60], ["p", "0102"], ["c", 60, 30]])

    def test_an_identical_file_is_one_copy(self):
        stock = synthetic_stock(120)
        self.assertEqual(self.roundtrip(stock, stock), [["c", 0, 120]])

    def test_a_shorter_target_with_a_range_removed(self):
        stock = synthetic_stock(300)
        self.roundtrip(stock, stock[:80] + stock[200:])

    def test_a_derived_put_carries_no_stock_window(self):
        rng = random.Random(9)
        stock = rng.randbytes(400)
        target = stock[:100] + rng.randbytes(37) + stock[100:250] + rng.randbytes(5) + stock[300:]
        ops = self.roundtrip(stock, target)
        gen.check_puts_clean(ops, [stock])  # the puts are random bytes of the target's own


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

    def test_nothing_is_written_when_a_put_carries_a_stock_window(self):
        out = self.tmp / "out"
        table = table_for([entry_for(self.stock_a, [["p", self.stock_a[5:15].hex()], ["c", 0, 300]], "0x1111111111111111")])
        with self.assertRaises(gen.PutCarriesStock):
            gen.generate(table, {"0x1111111111111111": self.stock_a}, out, resources=[self.stock_a])
        self.assertFalse(out.exists())

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


class DeriveTest(unittest.TestCase):
    """The table-level derive: it rebuilds each file's edits from a reference set and refuses what breaks the put rule."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="plugins-override-derive-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.stock = synthetic_stock(300, 4)
        self.name = "0x1111111111111111"
        entry = {**entry_for(self.stock, [["c", 0, 300]], self.name), "ops": []}
        self.table = table_for([entry], [own(NEW_ACTOR_HASH, "new_actor_hash")])

    def reference(self, target: bytes) -> Path:
        ref = self.tmp / "ref"
        ref.mkdir(exist_ok=True)
        (ref / f"{self.name}.{LEVEL}").write_bytes(target)
        return ref

    def test_the_derived_table_rebuilds_the_reference(self):
        target = self.stock[:100] + bytes.fromhex(NEW_ACTOR_HASH) + self.stock[100:] + b"\x07"
        derived = gen.derive(self.table, {self.name: self.stock}, self.reference(target), [self.stock])
        entry = derived["files"][0]
        self.assertEqual((entry["size"], entry["sha256"]), (len(target), gen.sha256(target)))
        self.assertEqual(gen.build_file(entry, self.stock), target)
        self.assertIn(["p", NEW_ACTOR_HASH], entry["ops"])

    def test_an_unlisted_value_of_ours_is_refused_not_written(self):
        target = self.stock[:100] + bytes.fromhex("0102030405060708") + self.stock[100:]
        with self.assertRaises(gen.PutNotOurs):
            gen.derive(self.table, {self.name: self.stock}, self.reference(target), [self.stock])

    def test_a_put_that_would_carry_a_run_derive_cannot_copy_from_is_refused(self):
        hidden = synthetic_stock(100, 8)  # a stock resource of the level that is not a file of the set
        target = self.stock + hidden[20:40]
        with self.assertRaises(gen.PutCarriesStock):
            gen.derive(self.table, {self.name: self.stock}, self.reference(target), [self.stock, hidden])

    def test_dump_and_load_round_trip(self):
        target = self.stock[:100] + bytes.fromhex(NEW_ACTOR_HASH) + self.stock[100:]
        derived = gen.derive(self.table, {self.name: self.stock}, self.reference(target), [self.stock])
        text = gen.dump_table(derived)
        self.assertEqual(json.loads(text), derived)
        self.assertEqual(gen.dump_table(json.loads(text)), text)


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


class LoadLevelTest(unittest.TestCase):
    def test_each_override_type_gets_the_stock_resource_of_its_own_stock_type(self):
        one, two, other = synthetic_stock(50, 11), synthetic_stock(60, 12), synthetic_stock(30, 13)
        a = entry_for(one, [["c", 0, 50]], "0x1111111111111111")
        b = {**entry_for(two, [["c", 0, 60]], "0x3333333333333333"), "stock_type": "0x4444444444444444"}
        seen = {}

        def fake_read_level(data_dir, level_hash):
            seen["call"] = (data_dir, level_hash)
            return {0x2222222222222222: [one], 0x4444444444444444: [two], 0x5555555555555555: [other]}

        with unittest.mock.patch.object(gen, "read_level", fake_read_level):
            stocks, resources = gen.load_level(table_for([a, b]), Path("somewhere"))
        self.assertEqual(stocks, {"0x1111111111111111": one, "0x3333333333333333": two})
        self.assertEqual(sorted(resources), sorted([one, two, other]))  # every resource of the level, not only the set's
        self.assertEqual(seen["call"], (Path("somewhere"), int(LEVEL, 16)))


@unittest.skipUnless(HAVE_ZSTD, "zstandard is not installed: the archive reader (rad_archive_tool) cannot run, so the read of a real archive is not tested")
class ReadLevelTest(unittest.TestCase):
    """read_level and the whole command, against a small archive the test packs with rad_archive_tool itself."""

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
            import rad_archive_tool as rad
            rad.repack_archive(self.tmp / "in", self.tmp / "game")
        self.data_dir = self.tmp / "game"

    def test_every_resource_of_the_level_is_read_and_no_other_level_is(self):
        level = gen.read_level(self.data_dir, self.level)
        self.assertEqual(level, {0x2222222222222222: [self.stock], 0x4444444444444444: [b"a type nobody asked for"]})

    def test_the_stock_resource_of_the_level_and_type_is_picked(self):
        got = gen.pick_stock(gen.read_level(self.data_dir, self.level), self.level, [0x2222222222222222])
        self.assertEqual(got, {0x2222222222222222: self.stock})

    def test_a_type_the_archive_does_not_have_is_refused(self):
        with self.assertRaises(gen.StockMissing):
            gen.pick_stock(gen.read_level(self.data_dir, self.level), self.level, [0x9999999999999999])

    def test_a_type_found_twice_is_refused(self):
        """Two manifests that both hold the level's resource of the type: which one is the game's is not known."""
        twin = self.tmp / "in2" / "efgh5678" / "0x2222222222222222"
        twin.mkdir(parents=True)
        (twin / f"{LEVEL}.bin").write_bytes(b"a second copy")
        import rad_archive_tool as rad
        with contextlib.redirect_stdout(io.StringIO()):
            rad.repack_archive(self.tmp / "in2", self.tmp / "game")
        with self.assertRaises(gen.StockMissing) as caught:
            gen.pick_stock(gen.read_level(self.data_dir, self.level), self.level, [0x2222222222222222])
        self.assertIn("2 resources", str(caught.exception))

    def test_a_folder_with_no_archive_is_refused(self):
        empty = self.tmp / "empty"
        empty.mkdir()
        with self.assertRaises(gen.StockMissing):
            gen.read_level(empty, self.level)

    def run_generate(self, patches: Path) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = gen.main(["generate", "--data-dir", str(self.data_dir), "--patches", str(patches), "--out", str(self.tmp / "out")])
        return code, out.getvalue(), err.getvalue()

    def test_the_command_writes_the_file_from_the_archive(self):
        ops = [["c", 0, 10], ["p", "a1a2"], ["c", 10, 290]]
        patches = self.tmp / "patches.json"
        patches.write_text(json.dumps(table_for([entry_for(self.stock, ops, "0x1111111111111111")])))
        code, out, _ = self.run_generate(patches)
        self.assertEqual(code, 0)
        self.assertEqual((self.tmp / "out" / f"0x1111111111111111.{LEVEL}").read_bytes(), self.stock[:10] + b"\xa1\xa2" + self.stock[10:])
        self.assertIn("wrote 1 files", out)

    def test_the_command_refuses_a_put_that_carries_a_run_of_another_resource_of_the_level(self):
        carried = b"a type nobody asked for"[:12]  # a stock run, in a resource that is not a file of the set
        ops = [["c", 0, 300], ["p", carried.hex()]]
        patches = self.tmp / "patches.json"
        patches.write_text(json.dumps(table_for([entry_for(self.stock, ops, "0x1111111111111111")], [own(carried.hex())])))
        code, _, err = self.run_generate(patches)
        self.assertEqual(code, gen.EXIT_REFUSED)
        self.assertIn("PutCarriesStock", err)
        self.assertFalse((self.tmp / "out").exists())

    def test_the_command_refuses_an_archive_of_another_game_version(self):
        entry = entry_for(synthetic_stock(300, 6), [["c", 0, 300]], "0x1111111111111111")
        patches = self.tmp / "patches.json"
        patches.write_text(json.dumps(table_for([entry])))
        code, _, err = self.run_generate(patches)
        self.assertEqual(code, gen.EXIT_REFUSED)
        self.assertIn("StockMismatch", err)
        self.assertFalse((self.tmp / "out").exists())


class RepositoryTableTest(unittest.TestCase):
    """The committed table against the rule, without the game."""

    @classmethod
    def setUpClass(cls):
        cls.table = gen.load_table()  # load_table runs check_table: every put of 8 bytes or more that can carry data is listed
        cls.ours = {o["hex"]: o for o in cls.table["ours"]}
        cls.puts = [(e["override_class"], bytes.fromhex(op[1])) for e in cls.table["files"] for op in e["ops"] if op[0] == "p"]

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
            total = sum(op[-1] if op[0] in ("c", "x") else 8 if op[0] == "h" else len(bytes.fromhex(op[1])) for op in e["ops"])
            self.assertEqual(total, e["size"], e["override_class"])

    def test_the_hashes_are_sha256_hex(self):
        for e in self.table["files"]:
            for key in ("stock_sha256", "sha256"):
                self.assertRegex(e[key], r"^[0-9a-f]{64}$", (e["override_class"], key))

    def test_the_only_values_the_table_lists_as_ours_are_the_known_ones(self):
        self.assertEqual({o["name"] for o in self.table["ours"]}, OURS_NAMES)

    def test_every_listed_value_is_used_by_a_put(self):
        used = {p.hex() for _, p in self.puts}
        for hex_value, o in self.ours.items():
            self.assertIn(hex_value, used, f"{o['name']} is listed but no put uses it")

    def test_the_new_actor_hash_and_the_component_id_are_the_only_8_byte_puts_that_can_carry_data(self):
        eight = {p.hex() for _, p in self.puts if len(p) == gen.WINDOW and gen.is_data(p)}
        self.assertEqual(eight, {NEW_ACTOR_HASH, PLUGINS_COMPONENT_ID})

    def test_the_component_id_is_in_the_page_element_file_only(self):
        where = [e["override_class"] for e in self.table["files"] if ["p", PLUGINS_COMPONENT_ID] in e["ops"]]
        self.assertEqual(where, ["CR15UIPage2ElementCR"])

    def test_every_longer_put_is_listed_text_or_floats(self):
        for cls, p in self.puts:
            if len(p) > gen.WINDOW and gen.is_data(p):
                self.assertIn(p.hex(), self.ours, f"{cls}: an unlisted put of {len(p)} bytes")

    def test_every_other_put_is_shorter_than_a_window_or_padding_or_a_count(self):
        for cls, p in self.puts:
            if p.hex() not in self.ours:
                self.assertTrue(len(p) < gen.WINDOW or not gen.is_data(p), f"{cls}: {p.hex()} is a put of {len(p)} bytes that can carry data")

    def test_the_text_values_are_ascii(self):
        for hex_value, o in self.ours.items():
            if o["name"].startswith("card_text"):
                bytes.fromhex(hex_value).decode("ascii")

    def test_the_puts_of_a_file_are_few_bytes(self):
        for e in self.table["files"]:
            n = sum(len(bytes.fromhex(op[1])) for op in e["ops"] if op[0] == "p")
            self.assertLessEqual(n, MAX_PUT_BYTES_PER_FILE, e["override_class"])

    def test_the_symbol_hashes_are_the_two_tile_names(self):
        named = {op[1] for e in self.table["files"] for op in e["ops"] if op[0] == "h"}
        self.assertEqual(named, SYMBOL_NAMES)
        self.assertEqual(set(self.table["symbols"]), SYMBOL_NAMES)

    def test_a_cross_copy_names_another_file_of_the_set(self):
        types = {e["override_type"] for e in self.table["files"]}
        for e in self.table["files"]:
            for op in e["ops"]:
                if op[0] == "x":
                    self.assertIn(op[1], types)
                    self.assertNotEqual(op[1], e["override_type"])

    def test_the_file_is_written_the_way_the_tool_writes_it(self):
        text = (ECHOMOD / "plugins_override_patches.json").read_text()
        self.assertEqual(gen.dump_table(json.loads(text)), text)

    def test_the_check_would_stop_a_table_with_a_pasted_value(self):
        broken = json.loads((ECHOMOD / "plugins_override_patches.json").read_text())
        broken["files"][0]["ops"].append(["p", "0102030405060708090a"])
        with self.assertRaises(gen.PutNotOurs):
            gen.check_table(broken)


@unittest.skipUnless(os.environ.get(DATA_DIR_ENV), f"{DATA_DIR_ENV} is not set: the real install was not read (not run in CI)")
class RealInstallTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data_dir = Path(os.environ[DATA_DIR_ENV])
        cls.table = gen.load_table()
        cls.stocks, cls.resources = gen.load_level(cls.table, cls.data_dir)

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="plugins-override-real-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_the_real_stock_resources_are_the_ones_the_table_was_made_from(self):
        for override_type, stock in self.stocks.items():
            e = next(f for f in self.table["files"] if f["override_type"] == override_type)
            self.assertEqual((len(stock), gen.sha256(stock)), (e["stock_size"], e["stock_sha256"]), e["stock_class"])

    def test_no_put_overlaps_a_stock_window_of_any_resource_of_the_level(self):
        checked = 0
        for e in self.table["files"]:
            for op in e["ops"]:
                if op[0] != "p":
                    continue
                raw = bytes.fromhex(op[1])
                for i in range(len(raw) - gen.WINDOW + 1):
                    window = raw[i:i + gen.WINDOW]
                    if gen.is_data(window):
                        checked += 1
                        self.assertFalse(any(window in res for res in self.resources), f"{e['override_class']}: put {op[1]} carries {window.hex()}")
        self.assertGreater(checked, 0, "no data window was checked: the test is not looking at anything")

    def test_a_put_holding_a_real_stock_run_is_caught(self):
        """The check itself: a stock run pasted into a put must be refused."""
        stock = next(iter(self.stocks.values()))
        run = next(stock[i:i + 12] for i in range(0, len(stock) - 12) if gen.is_data(stock[i:i + 8]))
        with self.assertRaises(gen.PutCarriesStock):
            gen.check_puts_clean([["p", run.hex()]], self.resources)

    def test_the_generated_files_are_the_recorded_ones(self):
        paths = gen.generate(self.table, self.stocks, self.tmp / "out", self.data_dir, resources=self.resources)
        self.assertEqual(len(paths), FILE_COUNT)
        recorded = {gen.file_name(e, LEVEL): e["sha256"] for e in self.table["files"]}
        self.assertEqual({p.name: gen.sha256(p.read_bytes()) for p in paths}, recorded)

    def test_the_generated_files_equal_the_finished_override_set(self):
        reference = os.environ.get(REFERENCE_ENV)
        if not reference:
            self.skipTest(f"{REFERENCE_ENV} is not set: the generated files were not compared with a finished override set")
        paths = gen.generate(self.table, self.stocks, self.tmp / "out", self.data_dir, resources=self.resources)
        for path in paths:
            self.assertEqual(path.read_bytes(), (Path(reference) / path.name).read_bytes(), path.name)

    def test_derive_from_the_finished_override_set_gives_the_committed_table(self):
        reference = os.environ.get(REFERENCE_ENV)
        if not reference:
            self.skipTest(f"{REFERENCE_ENV} is not set: the table was not rebuilt from a finished override set")
        derived = gen.derive(self.table, self.stocks, Path(reference), self.resources)
        self.assertEqual(gen.dump_table(derived), (ECHOMOD / "plugins_override_patches.json").read_text())


if __name__ == "__main__":
    unittest.main()
