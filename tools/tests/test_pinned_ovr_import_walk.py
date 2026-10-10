"""tools/pinned_ovr_import_walk.py on a hand-built AArch64 ELF (the real library is not in the repository)."""

import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "pinned_ovr_import_walk.py"
sys.path.insert(0, str(REPO / "tools"))
import pinned_ovr_import_walk as walker  # noqa: E402

PLT = 0x1000
TEXT = 0x2000
GOT = 0x3000
RET = 0xD65F03C0


def bl(at: int, to: int) -> int:
    return 0x94000000 | (((to - at) // 4) & 0x3FFFFFF)


def b(at: int, to: int) -> int:
    return 0x14000000 | (((to - at) // 4) & 0x3FFFFFF)


def stub(index: int) -> list:
    """adrp x16, GOT page; ldr x17, [x16, #slot]; add; br: the stub for GOT slot `index`."""
    return [0x90000010 | (((GOT >> 12) - (0x1000 >> 12)) & 3) << 29 | ((((GOT >> 12) - (0x1000 >> 12)) >> 2) << 5),
            0xF9400211 | (((index * 8) >> 3) << 10), 0x91000210, 0xD61F0220]


def build(names: list, text_words: list) -> bytes:
    """A minimal ELF64: .plt (PLT0 + one stub per name), .text, .rela.plt, .dynsym, .dynstr, .shstrtab."""
    dynstr = b"\0" + b"".join(n.encode() + b"\0" for n in names)
    str_offsets = []
    pos = 1
    for n in names:
        str_offsets.append(pos)
        pos += len(n) + 1
    dynsym = b"\0" * 24 + b"".join(struct.pack("<IBBHQQ", o, 0x12, 0, 0, 0, 0) for o in str_offsets)
    rela = b"".join(struct.pack("<QQq", GOT + i * 8, (i + 1) << 32 | 1026, 0) for i in range(len(names)))
    plt = b"\0" * 32 + b"".join(struct.pack("<4I", *stub(i)) for i in range(len(names)))
    text = struct.pack(f"<{len(text_words)}I", *text_words)
    shstr = b"\0.plt\0.text\0.rela.plt\0.dynsym\0.dynstr\0.shstrtab\0"
    name_at = {n: shstr.index(n.encode()) for n in (".plt", ".text", ".rela.plt", ".dynsym", ".dynstr", ".shstrtab")}
    blobs = [("", None), (".plt", plt), (".text", text), (".rela.plt", rela), (".dynsym", dynsym),
             (".dynstr", dynstr), (".shstrtab", shstr)]
    addresses = {".plt": PLT, ".text": TEXT}
    body = b""
    data_off = 64
    headers = []
    for name, blob in blobs:
        if blob is None:
            headers.append(struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
            continue
        off = data_off + len(body)
        body += blob
        headers.append(struct.pack("<IIQQQQIIQQ", name_at[name], 1, 0, addresses.get(name, 0), off, len(blob),
                                   0, 0, 4, 0))
    shoff = data_off + len(body)
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8
    header = ident + struct.pack("<HHIQQQIHHHHHH", 2, 183, 1, 0, 0, shoff, 0, 64, 0, 0, 64, len(blobs), len(blobs) - 1)
    return header + b"\0" * (data_off - len(header)) + body + b"".join(headers)


class WalkTest(unittest.TestCase):
    def write(self, names, words) -> Path:
        directory = Path(tempfile.mkdtemp(prefix="pinned-walk-"))
        self.addCleanup(lambda: [p.unlink() for p in directory.iterdir()] or directory.rmdir())
        path = directory / "lib.so"
        path.write_bytes(build(names, words))
        return path

    def stub_address(self, index: int) -> int:
        return PLT + 32 + index * 16

    def test_reaches_a_direct_import_a_callee_and_a_tail_call(self):
        # root: bl ovr_A; bl callee; ret      callee: b ovr_B (tail call)
        root, callee = TEXT, TEXT + 12
        words = [bl(root, self.stub_address(0)), bl(root + 4, callee), RET, b(callee, self.stub_address(1))]
        path = self.write(["ovr_A", "ovr_B", "ovr_Unreached"], words)
        elf = walker.Elf(path.read_bytes())
        self.assertEqual(sorted(walker.walk(elf, {"root": root})), ["ovr_A", "ovr_B"])

    def test_import_names_are_resolved_from_the_got_slot_not_the_order(self):
        root = TEXT
        words = [bl(root, self.stub_address(2)), RET]
        path = self.write(["ovr_first", "ovr_second", "ovr_third"], words)
        elf = walker.Elf(path.read_bytes())
        self.assertEqual(sorted(walker.walk(elf, {"root": root})), ["ovr_third"])

    def test_expect_file_flags_an_unhandled_import_and_a_stale_entry(self):
        words = [bl(TEXT, self.stub_address(0)), bl(TEXT + 4, self.stub_address(1)), RET]
        elf_path = self.write(["ovr_Known", "ovr_New"], words)
        expect = elf_path.parent / "expect.txt"
        expect.write_text("ovr_Known  # hooked\novr_Gone  # stale\n", encoding="utf-8")
        original_roots = dict(walker.ROOTS)
        try:
            walker.ROOTS.clear()
            walker.ROOTS["root"] = TEXT
            found = walker.reachable_ovr_imports(elf_path)
        finally:
            walker.ROOTS.clear()
            walker.ROOTS.update(original_roots)
        self.assertEqual(sorted(found), ["ovr_Known", "ovr_New"])

    def test_the_committed_expectation_file_is_well_formed(self):
        names = []
        for line in (REPO / "tools" / "pinned_ovr_imports.txt").read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                names.append(line.split()[0])
        self.assertEqual(len(names), len(set(names)), "a duplicate entry")
        for name in names:
            self.assertTrue(name.startswith(("ovr_", "ovrID_")), name)
        for name in ("ovr_PopMessage", "ovr_Message_GetType", "ovr_Message_GetRequestID", "ovr_FreeMessage",
                     "ovr_Message_GetError"):
            self.assertIn(name, names)

    def test_command_line_exit_codes_on_the_synthetic_library(self):
        words = [bl(TEXT, self.stub_address(0)), RET]
        path = self.write(["ovr_Known"], words)
        # The CLI uses the real roots, which point outside this tiny text section: nothing is reached.
        result = subprocess.run([sys.executable, str(TOOL), str(path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
