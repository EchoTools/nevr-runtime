"""tools/pe_exports.py: reads an export table, groups exports that share a body, spots stubs and forwarders.

A synthetic PE32+ image is built in memory so the tests need no game files."""
from __future__ import annotations

import pathlib
import struct
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import pe_exports  # noqa: E402

SECTION_VA = 0x1000
FILE_ALIGN = 0x200


def build_pe(exports: list[tuple[str, bytes | str]]) -> bytes:
    """One .text section holding code bodies and the export table. A str body is a forwarder."""
    code = bytearray()
    body_rva: dict[object, int] = {}
    for _, body in exports:
        if isinstance(body, bytes) and body not in body_rva:
            body_rva[body] = SECTION_VA + len(code)
            code += body + b"\xcc" * 8
    export_off = len(code)
    export_rva = SECTION_VA + export_off
    n = len(exports)
    names = sorted(exports, key=lambda e: e[0])
    strings = bytearray()
    forwarder_rva: dict[str, int] = {}
    table_size = 40 + 4 * n + 4 * n + 2 * n
    str_base = export_rva + table_size
    name_rvas = []
    for name, _ in names:
        name_rvas.append(str_base + len(strings))
        strings += name.encode() + b"\0"
    for _, body in names:
        if isinstance(body, str):
            forwarder_rva[body] = str_base + len(strings)
            strings += body.encode() + b"\0"
    func_rvas = [body_rva[b] if isinstance(b, bytes) else forwarder_rva[b] for _, b in names]
    addr_funcs = export_rva + 40
    addr_names = addr_funcs + 4 * n
    addr_ords = addr_names + 4 * n
    directory = struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, 0, 1, n, n, addr_funcs, addr_names, addr_ords)
    directory += struct.pack(f"<{n}I", *func_rvas) + struct.pack(f"<{n}I", *name_rvas)
    directory += struct.pack(f"<{n}H", *range(n)) + bytes(strings)
    section = bytes(code) + directory
    raw_size = (len(section) + FILE_ALIGN - 1) // FILE_ALIGN * FILE_ALIGN
    section = section.ljust(raw_size, b"\0")

    dos = bytearray(0x40)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    opt = bytearray(240)
    struct.pack_into("<H", opt, 0, 0x20B)
    struct.pack_into("<II", opt, 112, export_rva, len(directory))        # data directory 0
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, len(opt), 0x2022)
    section_hdr = struct.pack("<8sIIIIIIHHI", b".text", len(section), SECTION_VA, raw_size, 0x400, 0, 0, 0, 0,
                              0x60000020)
    image = bytes(dos) + b"PE\0\0" + coff + bytes(opt) + section_hdr
    return image.ljust(0x400, b"\0") + section


RET0 = bytes.fromhex("31c0c3")
BODY_A = bytes.fromhex("488b0529db2e00c3")


class ParseExportsTest(unittest.TestCase):
    def setUp(self):
        self.exports = pe_exports.parse_exports(build_pe([
            ("MicRead", RET0), ("MicCreate", RET0), ("Friends", BODY_A), ("Fwd", "other.Real"),
        ]))
        self.by_name = {e.name: e for e in self.exports}

    def test_names_are_listed_sorted_with_their_rvas(self):
        self.assertEqual([e.name for e in self.exports], ["Friends", "Fwd", "MicCreate", "MicRead"])
        self.assertEqual(self.by_name["MicRead"].rva, self.by_name["MicCreate"].rva)
        self.assertNotEqual(self.by_name["Friends"].rva, self.by_name["MicRead"].rva)

    def test_exports_sharing_a_body_form_a_group(self):
        self.assertEqual(self.by_name["MicRead"].group, 2)
        self.assertEqual(self.by_name["Friends"].group, 1)
        folded = pe_exports.folded_groups(self.exports)
        self.assertEqual(list(folded.values()), [["MicCreate", "MicRead"]])

    def test_trivial_stubs_are_named_and_real_bodies_are_not(self):
        self.assertEqual(self.by_name["MicRead"].stub, "return 0")
        self.assertIsNone(self.by_name["Friends"].stub)
        self.assertTrue(self.by_name["Friends"].head.startswith("488b0529"))

    def test_a_forwarder_is_reported_and_has_no_body(self):
        self.assertEqual(self.by_name["Fwd"].forwarder, "other.Real")
        self.assertEqual(self.by_name["Fwd"].head, "")
        self.assertNotIn("Fwd", [n for names in pe_exports.folded_groups(self.exports).values() for n in names])

    def test_compare_splits_the_names(self):
        other = pe_exports.parse_exports(build_pe([("Friends", BODY_A), ("Social", BODY_A)]))
        only_a, only_b, both = pe_exports.compare(self.exports, other)
        self.assertEqual((only_a, only_b, both), (["Fwd", "MicCreate", "MicRead"], ["Social"], ["Friends"]))

    def test_non_pe_input_is_an_error(self):
        with self.assertRaises(pe_exports.PeError):
            pe_exports.parse_exports(b"not a pe file")
        with self.assertRaises(pe_exports.PeError):
            pe_exports.parse_exports(b"MZ" + b"\0" * 0x80)

    def test_table_output_lists_every_export_and_the_shared_bodies(self):
        text = pe_exports.render_table("x.dll", self.exports)
        self.assertIn("x.dll: 4 named exports", text)
        self.assertIn("MicCreate, MicRead", text)
        self.assertIn("-> other.Real", text)


if __name__ == "__main__":
    unittest.main()
