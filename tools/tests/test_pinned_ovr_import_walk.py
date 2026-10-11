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


def build(names: list, text_words: list, functions: tuple = ()) -> bytes:
    """A minimal ELF64: .plt (PLT0 + one stub per name), .text, .rela.plt, .dynsym, .dynstr, .shstrtab.

    `functions` are (name, address, size) exported symbols after the imports, for the call-site check."""
    all_names = list(names) + [f[0] for f in functions]
    dynstr = b"\0" + b"".join(n.encode() + b"\0" for n in all_names)
    str_offsets = []
    pos = 1
    for n in all_names:
        str_offsets.append(pos)
        pos += len(n) + 1
    dynsym = b"\0" * 24 + b"".join(struct.pack("<IBBHQQ", o, 0x12, 0, 0, 0, 0) for o in str_offsets[:len(names)])
    dynsym += b"".join(struct.pack("<IBBHQQ", o, 0x12, 0, 1, f[1], f[2])
                       for o, f in zip(str_offsets[len(names):], functions))
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
    def write(self, names, words, functions=()) -> Path:
        directory = Path(tempfile.mkdtemp(prefix="pinned-walk-"))
        self.addCleanup(lambda: [p.unlink() for p in directory.iterdir()] or directory.rmdir())
        path = directory / "lib.so"
        path.write_bytes(build(names, words, functions))
        return path

    def sites_fixture(self, listed: str, header_values: str = "0x2004, 0x200c"):
        """A library with a login function and a Social function calling ovr_User_GetOrgScopedID, and the files."""
        # login (0x2000): bl GetOrgScopedID @0x2000; ret @0x2004... the return address is site + 4.
        # social (0x2010): bl GetOrgScopedID @0x2010.
        login, social = TEXT, TEXT + 0x10
        words = [bl(login, self.stub_address(0)), RET, RET, RET, bl(social, self.stub_address(0)), RET]
        path = self.write(["ovr_User_GetOrgScopedID"], words,
                          (("_ZN10NRadEngine10CNSOVRUser13LogInInternalERKNS_5CJsonE", login, 8),
                           ("_ZN10NRadEngine12CNSOVRSocial8JoinedCBEP10ovrMessage", social, 8)))
        sites = path.parent / "sites.txt"
        sites.write_text(listed, encoding="utf-8")
        header = path.parent / "targets.h"
        header.write_text("inline constexpr std::uint64_t kOrgRequestLoginReturns[] = {%s};\n" % header_values,
                          encoding="utf-8")
        return path, sites, header

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

    def run_sites(self, path, sites, header):
        return subprocess.run([sys.executable, "-I", str(TOOL), str(path), "--sites", str(sites), "--header", str(header)],
                              capture_output=True, text=True)

    def test_sites_accepts_a_fully_classified_library_and_a_matching_header(self):
        path, sites, header = self.sites_fixture(
            "ovr_User_GetOrgScopedID 0x2000 login\novr_User_GetOrgScopedID 0x2010 social\n", "0x2004")
        result = self.run_sites(path, sites, header)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 request call sites (1 login, 1 forwarded)", result.stdout)

    def test_sites_flags_a_new_caller_nobody_classified(self):
        path, sites, header = self.sites_fixture("ovr_User_GetOrgScopedID 0x2000 login\n", "0x2004")
        result = self.run_sites(path, sites, header)
        self.assertEqual(result.returncode, 1)
        self.assertIn("UNCLASSIFIED: ovr_User_GetOrgScopedID is called at 0x2010 in "
                      "_ZN10NRadEngine12CNSOVRSocial8JoinedCB", result.stderr)

    def test_sites_flags_a_social_caller_listed_as_login(self):
        path, sites, header = self.sites_fixture(
            "ovr_User_GetOrgScopedID 0x2000 login\novr_User_GetOrgScopedID 0x2010 login\n", "0x2004, 0x2014")
        result = self.run_sites(path, sites, header)
        self.assertEqual(result.returncode, 1)
        self.assertIn("MISCLASSIFIED: ovr_User_GetOrgScopedID at 0x2010", result.stderr)

    def test_sites_flags_a_stale_entry(self):
        path, sites, header = self.sites_fixture(
            "ovr_User_GetOrgScopedID 0x2000 login\novr_User_GetOrgScopedID 0x2010 social\n"
            "ovr_User_GetOrgScopedID 0x2020 social\n", "0x2004")
        result = self.run_sites(path, sites, header)
        self.assertEqual(result.returncode, 1)
        self.assertIn("STALE: ovr_User_GetOrgScopedID at 0x2020", result.stderr)

    def test_sites_flags_a_header_that_drifted_from_the_login_sites(self):
        path, sites, header = self.sites_fixture(
            "ovr_User_GetOrgScopedID 0x2000 login\novr_User_GetOrgScopedID 0x2010 social\n", "0x2004, 0x2014")
        result = self.run_sites(path, sites, header)
        self.assertEqual(result.returncode, 1)
        self.assertIn("DRIFT: kOrgRequestLoginReturns", result.stderr)

    def test_the_committed_sites_file_matches_the_committed_header(self):
        listed = []
        for line in (REPO / "tools" / "pinned_ovr_sites.txt").read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                name, site, klass = line.split()
                self.assertIn(klass, ("login", "social"))
                if name == "ovr_User_GetOrgScopedID" and klass == "login":
                    listed.append(int(site, 16) + 4)
        header = (REPO / "src" / "quest" / "login" / "login_prerequisite_targets.h").read_text(encoding="utf-8")
        marker = header.index("kOrgRequestLoginReturns")
        body = header[header.index("{", marker) + 1:header.index("}", marker)]
        import re
        self.assertEqual(sorted(listed), sorted(int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", body)))


if __name__ == "__main__":
    unittest.main()
