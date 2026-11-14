"""The runtime md5 pin (#1737): tools/quest_runtime_pin.py replaces the stock entry of the reference runtime's libr15
checksum list, in both forms the binary holds it in (12 or 13 ASCII copies by runtime, as many sites of sixteen immediate words), with the
md5 of the libr15 the APK carries.

The fixture is a synthetic AArch64 ELF built here (`make_runtime`): an executable segment with one site per module per entry, each of
sixteen `movz`/`movk` words that build the four 8-byte chunks of the entry's text (interleaved over four registers, with
unrelated instructions between), and a data segment with one list of three entries per module. A pin of the fixture must equal the
fixture built for the new text, for the 12-module and the 13-module runtime. One real-file case runs only when
NEVR_REFERENCE_LIBVRAPI names the `libvrapi.so` of either reference runtime (REAL_RUNTIMES, by sha-256).
"""
from __future__ import annotations

import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import quest_runtime_pin as pin  # noqa: E402
import work_root  # noqa: E402

TOOL = REPO / "tools" / "quest_runtime_pin.py"
OLD = "4911ef7f1248580ee2978590d581dbfd"  # the stock libr15's md5: index 0 of the reference runtime's list
SECOND = "6e63ac1285105a018a82e39cd1e3edf0"
THIRD = "0734f86fd23afc03aea76ffadeb7b924"
NEW = hashlib.md5(b"a patched libr15").hexdigest()
OTHER = hashlib.md5(b"another libr15").hexdigest()
NOP = 0xD503201F
MODULES = 13  # the modules wave 2 of the reference runtime gates on the list (its own notes say thirteen lists)
PINNED_REFERENCE_MODULES = 12  # the modules the pinned reference APK's runtime (libvrapi sha-256 59a3cd20...) gates
EOR_X8 = 0xCA090108  # eor x8, x8, x9: an instruction on register 8 that is neither movz nor movk
# The two real runtimes: libvrapi sha-256 -> (modules, bytes a pin to LANE_PIN_MD5 changes, sha-256 of the pinned file).
REAL_RUNTIMES = {
    "59a3cd2090b29f72cc14b853f1550073c9dd066c87c9cd120eed6d93f84c3839":  # the pinned reference APK (d0b51a01...)
        (PINNED_REFERENCE_MODULES, 828, "a266784959fcf2583a1ff88974a345a20999134719491dd334277433688d42c9"),
    "1212fcea7837111d7ae8ea835f64399f479a50cde89a0f44a003caedae12466a":  # the wave-2 runtime, libvrapi 8b7eab33
        (MODULES, 897, "f7af78d1ee00bd59650e4536a8b903c0776566909fb0f8034da9e8ee404318c1"),
}
LANE_PIN_MD5 = "3e89307b9eddca58aa4573953a4477b9"


def movz(rd: int, imm: int, hw: int = 0) -> int:
    return 0xD2800000 | hw << 21 | imm << 5 | rd


def movk(rd: int, imm: int, hw: int) -> int:
    return 0xF2800000 | hw << 21 | imm << 5 | rd


def site_words(text: str) -> list[int]:
    """The sixteen words one module's compare builds `text` with: movz of the low half then the three movk, per chunk,
    interleaved over x8..x11 the way the compiler orders them, with an unrelated instruction on x8 inside the run."""
    chunks = [struct.unpack("<Q", text.encode()[i:i + 8])[0] for i in range(0, 32, 8)]
    words = []
    for hw in range(4):
        for k, chunk in enumerate(chunks):
            half = (chunk >> (16 * hw)) & 0xFFFF
            words.append(movz(8 + k, half) if hw == 0 else movk(8 + k, half, hw))
        words.append(EOR_X8)
    return words  # 16 immediates and 4 unrelated words


def make_elf(code: bytes, data: bytes, machine: int = 0xB7) -> bytes:
    """An AArch64 ELF64 with an R+X segment holding `code` and an R segment holding `data`."""
    code_at, data_at = 0x1000, 0x1000 + ((len(code) + 0xFFF) & ~0xFFF)
    header = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01" + b"\0" * 9, 3, machine, 1, 0, 64, 0, 0, 64, 56, 2, 64, 0, 0)
    phdrs = struct.pack("<IIQQQQQQ", 1, 5, code_at, code_at, code_at, len(code), len(code), 0x1000)
    phdrs += struct.pack("<IIQQQQQQ", 1, 4, data_at, data_at, data_at, len(data), len(data), 0x1000)
    image = bytearray(header + phdrs)
    image.extend(b"\0" * (code_at - len(image)))
    image.extend(code)
    image.extend(b"\0" * (data_at - len(image)))
    image.extend(data)
    return bytes(image)


def make_runtime(old: str = OLD, *, modules: int = MODULES, sites: int | None = None, copies: int | None = None,
                 others: tuple[str, ...] = (SECOND, THIRD), decoy: bool = False, extra: bytes = b"") -> bytes:
    """A runtime fixture of `modules` gated modules whose list is `old` then `others`. `sites` sites and `copies` ASCII copies
    of `old` (`modules` each unless given); the others always have `modules` of each. `decoy` adds a run of `old` in the
    non-executable segment."""
    sites = modules if sites is None else sites
    copies = modules if copies is None else copies
    code = bytearray()
    for m in range(max(sites, modules)):
        for text, wanted in [(old, sites)] + [(o, modules) for o in others]:
            if m < wanted:
                code += b"".join(struct.pack("<I", w) for w in site_words(text))
                code += struct.pack("<I", NOP) * 12
    data = bytearray()
    for m in range(max(copies, modules)):
        for text, wanted in [(old, copies)] + [(o, modules) for o in others]:
            if m < wanted:
                data += text.encode() + b"\0"
        data += b"module %d: %%.8s is none of the known builds\0" % m
    if decoy:
        data += b"\0" * (-len(data) % 4)  # a word-aligned run, as a compiler would leave it
        data += b"".join(struct.pack("<I", w) for w in site_words(old))
    data += extra
    return make_elf(bytes(code), bytes(data))


class PinDataTest(unittest.TestCase):
    def test_each_known_runtime_pins_with_its_own_number_of_modules(self):
        self.assertEqual(pin.KNOWN_SITES, (PINNED_REFERENCE_MODULES, MODULES))
        for modules in pin.KNOWN_SITES:
            with self.subTest(modules=modules):
                runtime = make_runtime(modules=modules)
                self.assertTrue(pin.carries_table(runtime))
                result, report = pin.pin_data(runtime, NEW)
                self.assertEqual(result, make_runtime(NEW, modules=modules))
                self.assertEqual((report["ascii_copies"], report["sites"], report["immediate_runs"], report["immediate_words"]),
                                 (modules, modules, 4 * modules, 16 * modules))
                again, _ = pin.pin_data(result, NEW)
                self.assertEqual(again, result)

    def test_a_runtime_of_another_number_of_modules_is_not_a_runtime_and_is_refused(self):
        for modules in (PINNED_REFERENCE_MODULES - 1, MODULES + 1):
            with self.subTest(modules=modules):
                runtime = make_runtime(modules=modules)
                self.assertFalse(pin.carries_table(runtime))
                with self.assertRaisesRegex(pin.PinError, "holds no checksum table"):
                    pin.pin_data(runtime, NEW)

    def test_copies_and_sites_of_two_known_counts_do_not_mix(self):
        for copies, sites in ((PINNED_REFERENCE_MODULES, MODULES), (MODULES, PINNED_REFERENCE_MODULES)):
            with self.subTest(copies=copies, sites=sites):
                with self.assertRaisesRegex(pin.PinError, f"has {4 * sites} immediate runs .* not {4 * copies}"):
                    pin.pin_data(make_runtime(copies=copies, sites=sites), NEW)

    def test_the_pinned_runtime_is_the_runtime_built_for_the_new_md5(self):
        result, report = pin.pin_data(make_runtime(), NEW)
        self.assertEqual(result, make_runtime(NEW))
        self.assertEqual((report["ascii_copies"], report["sites"], report["immediate_runs"], report["immediate_words"]), (MODULES, MODULES, 4 * MODULES, 16 * MODULES))
        self.assertEqual(report["old_md5"], OLD)
        self.assertEqual(report["table"], [OLD, SECOND, THIRD])

    def test_only_the_copies_and_the_immediate_fields_change(self):
        before = make_runtime()
        result, report = pin.pin_data(before, NEW)
        self.assertEqual(len(result), len(before))
        differing = [i for i in range(len(before)) if before[i] != result[i]]
        self.assertEqual(len(differing), report["changed_bytes"])
        text = pin.ascii_copies(before, OLD.encode())
        self.assertEqual(len(text), MODULES)
        inside = {i for o in text for i in range(o, o + 32)}
        for i in differing:
            if i in inside:
                continue
            word = i & ~3
            self.assertEqual((struct.unpack_from("<I", before, word)[0] ^ struct.unpack_from("<I", result, word)[0]) & ~(0xFFFF << 5), 0)

    def test_the_new_md5_is_found_once_per_module_in_each_form_and_the_old_one_never(self):
        result, _ = pin.pin_data(make_runtime(), NEW)
        scan = pin.Scan(result)
        self.assertEqual(len(pin.ascii_copies(result, NEW.encode())), MODULES)
        self.assertEqual(sum(len(scan.runs(c)) for c in pin.chunks_of(NEW.encode())), 4 * MODULES)
        self.assertEqual(pin.ascii_copies(result, OLD.encode()), [])
        self.assertEqual(sum(len(scan.runs(c)) for c in pin.chunks_of(OLD.encode())), 0)
        self.assertEqual(pin.table(result), [NEW.encode(), SECOND.encode(), THIRD.encode()])

    def test_the_other_entries_keep_their_copies_and_sites(self):
        result, _ = pin.pin_data(make_runtime(), NEW)
        scan = pin.Scan(result)
        for text in (SECOND, THIRD):
            self.assertEqual(len(pin.ascii_copies(result, text.encode())), MODULES)
            self.assertEqual(sum(len(scan.runs(c)) for c in pin.chunks_of(text.encode())), 4 * MODULES)

    def test_a_second_pin_with_the_same_md5_changes_nothing(self):
        once, _ = pin.pin_data(make_runtime(), NEW)
        twice, report = pin.pin_data(once, NEW)
        self.assertEqual(twice, once)
        self.assertEqual(report["changed_bytes"], 0)

    def test_a_file_pinned_to_another_libr15_is_refused_naming_both_entries(self):
        once, _ = pin.pin_data(make_runtime(), NEW)
        with self.assertRaises(pin.WrongEntry) as caught:
            pin.pin_data(once, OTHER)
        self.assertEqual((caught.exception.expected, caught.exception.found), (OLD, NEW))

    def test_a_decoy_run_in_a_data_segment_is_not_a_site(self):
        before = make_runtime(decoy=True)
        result, report = pin.pin_data(before, NEW)
        self.assertEqual(report["sites"], MODULES)
        decoy = b"".join(struct.pack("<I", w) for w in site_words(OLD))
        self.assertTrue(before.endswith(decoy))
        self.assertEqual(len(pin.Scan(before).runs(pin.chunks_of(OLD.encode())[0])), MODULES)
        self.assertTrue(result.endswith(decoy))  # not an executable segment: left as it was

    def test_twelve_sites_against_thirteen_copies_are_refused(self):
        with self.assertRaisesRegex(pin.PinError, "has 48 immediate runs"):
            pin.pin_data(make_runtime(sites=12), NEW)

    def test_a_duplicated_site_is_refused(self):
        with self.assertRaisesRegex(pin.PinError, "has 56 immediate runs"):
            pin.pin_data(make_runtime(sites=14), NEW)

    def test_eleven_ascii_copies_are_refused(self):
        with self.assertRaisesRegex(pin.PinError, r"has 11 ASCII copies, not one of \(12, 13\)"):
            pin.pin_data(make_runtime(copies=11), NEW)

    def test_a_text_inside_a_longer_hex_string_is_not_a_copy_and_is_left_alone(self):
        for extra in (b"0" + OLD.encode() + b"\0", OLD.encode() + b"0\0"):
            result, report = pin.pin_data(make_runtime(extra=extra), NEW)
            self.assertEqual(report["ascii_copies"], MODULES)
            self.assertEqual(result, make_runtime(NEW, extra=extra))

    def test_a_new_md5_that_is_another_entry_of_the_list_is_refused(self):
        with self.assertRaisesRegex(pin.PinError, "already in the file"):
            pin.pin_data(make_runtime(), THIRD)

    def test_a_new_md5_present_once_in_ascii_is_refused(self):
        with self.assertRaisesRegex(pin.PinError, "already in the file .1 ASCII copies, 0 immediate runs"):
            pin.pin_data(make_runtime(extra=NEW.encode() + b"\0"), NEW)

    def test_a_new_md5_present_as_immediate_runs_is_refused(self):
        runtime = make_runtime()
        spliced = runtime.replace(b"".join(struct.pack("<I", w) for w in site_words(THIRD)),
                                  b"".join(struct.pack("<I", w) for w in site_words(NEW)), 1)
        self.assertNotEqual(spliced, runtime)
        with self.assertRaisesRegex(pin.PinError, "already in the file .0 ASCII copies, 4 immediate runs"):
            pin.pin_data(spliced, NEW)

    def test_a_file_with_no_list_is_not_the_runtime(self):
        plain = make_elf(struct.pack("<II", NOP, NOP), b"nothing here\0")
        self.assertFalse(pin.carries_table(plain))
        with self.assertRaisesRegex(pin.PinError, "not the reference runtime"):
            pin.pin_data(plain, NEW)

    def test_a_file_that_is_not_an_aarch64_elf_is_refused(self):
        with self.assertRaisesRegex(pin.PinError, "not a 64-bit little-endian ELF"):
            pin.pin_data(b"not an elf " + (OLD.encode() + b"\0") * MODULES, NEW)
        with self.assertRaisesRegex(pin.PinError, "is not AArch64"):
            pin.pin_data(make_elf(b"\0" * 8, (OLD.encode() + b"\0") * MODULES, machine=0x3E), NEW)

    def test_a_replace_index_outside_the_list_is_refused(self):
        with self.assertRaisesRegex(pin.PinError, "the table has 3 entries"):
            pin.pin_data(make_runtime(), NEW, 3)

    def test_the_replace_index_and_the_expected_md5_pick_the_entry(self):
        result, report = pin.pin_data(make_runtime(), NEW, 1, SECOND)
        self.assertEqual(report["old_md5"], SECOND)
        self.assertEqual(result, make_runtime(OLD, others=(NEW, THIRD)))

    def test_a_damaged_first_entry_is_refused_and_does_not_retarget_the_index(self):
        damaged = make_runtime().replace(OLD.encode(), b"x" * 32, 1)  # the first ASCII copy: the list now opens with SECOND
        self.assertEqual(pin.table(damaged)[0], SECOND.encode())
        self.assertTrue(pin.carries_table(damaged))
        with self.assertRaises(pin.WrongEntry) as caught:
            pin.pin_data(damaged, NEW)
        self.assertEqual((caught.exception.index, caught.exception.expected, caught.exception.found), (0, OLD, SECOND))
        self.assertIn(OLD, str(caught.exception))
        self.assertIn(SECOND, str(caught.exception))

    def test_a_wrong_expected_md5_is_refused(self):
        with self.assertRaises(pin.WrongEntry) as caught:
            pin.pin_data(make_runtime(), NEW, 0, SECOND)
        self.assertEqual((caught.exception.expected, caught.exception.found), (SECOND, OLD))

    def test_an_expected_md5_that_is_not_32_lowercase_hex_is_refused(self):
        with self.assertRaises(pin.PinError):
            pin.pin_data(make_runtime(), NEW, 0, "4911ef7f")

    def test_the_expected_entry_must_have_its_immediate_sites_too(self):
        # the ASCII table says OLD is at index 0 and it has 13 copies; one of its sites is gone, so the pin must not go on
        runtime = make_runtime()
        site = b"".join(struct.pack("<I", w) for w in site_words(OLD))
        damaged = runtime.replace(site, b"".join(struct.pack("<I", NOP) for _ in site_words(OLD)), 1)
        self.assertNotEqual(damaged, runtime)
        with self.assertRaisesRegex(pin.PinError, "has 48 immediate runs"):
            pin.pin_data(damaged, NEW)

    def test_an_md5_that_is_not_32_lowercase_hex_is_refused(self):
        for bad in ("", "3E89307B9EDDCA58AA4573953A4477B9", NEW[:-1], NEW + "0", "g" * 32):
            with self.assertRaises(pin.PinError, msg=bad):
                pin.pin_data(make_runtime(), bad)

    def test_the_store_style_library_carries_no_list(self):
        self.assertFalse(pin.carries_table(make_elf(b"\0" * 8, b"vrapi\0" + (OLD.encode() + b"\0") * 3)))


class PinCommandTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-runtime-pin-", dir=work_root.test_scratch())
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.libvrapi = self.tmp / "libvrapi.so"
        self.libvrapi.write_bytes(make_runtime())
        self.libr15 = self.tmp / "libr15.so"
        self.libr15.write_bytes(b"a patched libr15")  # its md5 is NEW

    def run_tool(self, *args: str):
        return subprocess.run([sys.executable, "-I", str(TOOL), *args], capture_output=True, text=True)

    def test_pin_hashes_the_libr15_file_and_logs_one_json_line(self):
        out = self.tmp / "pinned.so"
        result = self.run_tool("pin", "--libvrapi", str(self.libvrapi), "--libr15", str(self.libr15), "--out", str(out))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(out.read_bytes(), make_runtime(NEW))
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 1)
        record = json.loads(lines[0])
        self.assertEqual(record["tag"], "NEVR.QUEST.RUNTIME_PIN")
        self.assertEqual(record["event"], "runtime_pinned")
        self.assertEqual((record["libr15_md5"], record["old_md5"], record["new_md5"]), (NEW, OLD, NEW))
        self.assertEqual(record["libvrapi_sha256_out"], hashlib.sha256(out.read_bytes()).hexdigest())
        self.assertEqual(record["libvrapi_sha256_in"], hashlib.sha256(self.libvrapi.read_bytes()).hexdigest())
        self.assertEqual(self.libvrapi.read_bytes(), make_runtime())  # the input is left as it was

    def test_pin_twice_is_a_no_op_that_says_so(self):
        out, again = self.tmp / "pinned.so", self.tmp / "again.so"
        self.assertEqual(self.run_tool("pin", "--libvrapi", str(self.libvrapi), "--libr15", str(self.libr15), "--out", str(out)).returncode, 0)
        result = self.run_tool("pin", "--libvrapi", str(out), "--libr15", str(self.libr15), "--out", str(again))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["event"], "runtime_pin_unchanged")
        self.assertEqual(again.read_bytes(), out.read_bytes())

    def test_a_refusal_exits_one_logs_the_reason_on_stderr_and_writes_nothing(self):
        self.libvrapi.write_bytes(make_runtime(sites=12))
        out = self.tmp / "pinned.so"
        result = self.run_tool("pin", "--libvrapi", str(self.libvrapi), "--libr15", str(self.libr15), "--out", str(out))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        record = json.loads(result.stderr)
        self.assertEqual((record["tag"], record["event"]), ("NEVR.QUEST.RUNTIME_PIN", "runtime_pin_refused"))
        self.assertIn("48 immediate runs", record["reason"])
        self.assertFalse(out.exists())

    def test_a_damaged_first_entry_exits_one_naming_the_expected_and_the_found_md5(self):
        self.libvrapi.write_bytes(make_runtime().replace(OLD.encode(), b"x" * 32, 1))
        out = self.tmp / "pinned.so"
        result = self.run_tool("pin", "--libvrapi", str(self.libvrapi), "--libr15", str(self.libr15), "--out", str(out))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        record = json.loads(result.stderr)
        self.assertEqual(record["event"], "runtime_pin_refused")
        self.assertEqual((record["expected_old_md5"], record["found_md5"], record["replace_index"]), (OLD, SECOND, 0))
        self.assertFalse(out.exists())

    def test_a_wrong_expect_old_md5_exits_one(self):
        out = self.tmp / "pinned.so"
        result = self.run_tool("pin", "--libvrapi", str(self.libvrapi), "--libr15", str(self.libr15), "--out", str(out),
                               "--expect-old-md5", THIRD)
        self.assertEqual(result.returncode, 1)
        record = json.loads(result.stderr)
        self.assertEqual((record["expected_old_md5"], record["found_md5"]), (THIRD, OLD))
        self.assertFalse(out.exists())

    def test_expect_old_md5_and_replace_index_together_pin_another_entry(self):
        out = self.tmp / "pinned.so"
        result = self.run_tool("pin", "--libvrapi", str(self.libvrapi), "--libr15", str(self.libr15), "--out", str(out),
                               "--replace-index", "2", "--expect-old-md5", THIRD)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(out.read_bytes(), make_runtime(OLD, others=(SECOND, NEW)))

    def test_an_out_that_is_the_input_is_refused(self):
        result = self.run_tool("pin", "--libvrapi", str(self.libvrapi), "--libr15", str(self.libr15), "--out", str(self.libvrapi))
        self.assertEqual(result.returncode, 1)
        self.assertIn("--out is the input file", json.loads(result.stderr)["reason"])
        self.assertEqual(self.libvrapi.read_bytes(), make_runtime())

    def test_a_missing_libr15_is_refused(self):
        result = self.run_tool("pin", "--libvrapi", str(self.libvrapi), "--libr15", str(self.tmp / "none.so"), "--out", str(self.tmp / "o.so"))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stderr)["event"], "runtime_pin_refused")

    def test_detect_says_runtime_with_exit_zero_and_another_library_with_exit_three(self):
        found = self.run_tool("detect", "--libvrapi", str(self.libvrapi))
        self.assertEqual(found.returncode, 0, found.stderr)
        self.assertEqual(json.loads(found.stdout)["event"], "runtime_detected")
        self.assertEqual(json.loads(found.stdout)["table"], [OLD, SECOND, THIRD])
        other = self.tmp / "store.so"
        other.write_bytes(make_elf(b"\0" * 8, b"vrapi\0"))
        absent = self.run_tool("detect", "--libvrapi", str(other))
        self.assertEqual(absent.returncode, 3)
        self.assertEqual(json.loads(absent.stdout)["event"], "runtime_absent")

    def test_detect_refuses_a_file_that_is_not_an_aarch64_elf(self):
        other = self.tmp / "text.txt"
        other.write_bytes(b"hello")
        result = self.run_tool("detect", "--libvrapi", str(other))
        self.assertEqual(result.returncode, 1)


@unittest.skipUnless(os.environ.get("NEVR_REFERENCE_LIBVRAPI"),
                     "set NEVR_REFERENCE_LIBVRAPI to a reference runtime's lib/arm64-v8a/libvrapi.so (REAL_RUNTIMES)")
class RealReferenceRuntimeTest(unittest.TestCase):
    def test_the_real_runtime_pins_with_its_modules_and_to_the_known_bytes(self):
        data = pathlib.Path(os.environ["NEVR_REFERENCE_LIBVRAPI"]).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        self.assertIn(digest, REAL_RUNTIMES)
        modules, changed, pinned = REAL_RUNTIMES[digest]
        self.assertEqual(pin.table(data), [OLD.encode(), SECOND.encode(), THIRD.encode()])
        result, report = pin.pin_data(data, LANE_PIN_MD5)
        self.assertEqual((report["ascii_copies"], report["sites"], report["immediate_runs"], report["immediate_words"]),
                         (modules, modules, 4 * modules, 16 * modules))
        self.assertEqual(report["changed_bytes"], changed)
        self.assertEqual(hashlib.sha256(result).hexdigest(), pinned)
        again, _ = pin.pin_data(result, LANE_PIN_MD5)
        self.assertEqual(again, result)


if __name__ == "__main__":
    unittest.main()
