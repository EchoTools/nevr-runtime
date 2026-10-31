"""A stripped PE and its split-off symbol file match by .gnu_debuglink name and CRC32 (tools/verify_symbols.py).

StrippedBinaryMatchesItsSymbolsTest puts a tiny mingw executable through the same objcopy/strip sequence the
dist target in CMakeLists.txt runs and skips when no mingw compiler is installed. SyntheticPairTest builds
minimal PE images in the test, needs no toolchain and never reads a build output."""
from __future__ import annotations

import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "verify_symbols.py"
GCC = shutil.which("x86_64-w64-mingw32-gcc")
OBJCOPY = shutil.which("x86_64-w64-mingw32-objcopy")
STRIP = shutil.which("x86_64-w64-mingw32-strip")


def verify(binary: Path, symbols: Path):
    return subprocess.run(["python3", "-I", str(TOOL), str(binary), str(symbols)], capture_output=True, text=True)


class DistCmakeSequenceTest(unittest.TestCase):
    def test_dist_splits_symbols_before_stripping_and_links_after(self):
        cmake = (REPO / "CMakeLists.txt").read_text()
        section = cmake.split("# Distribution Package Target", 1)[1].split("# Legacy Distribution Package Target", 1)[0]
        for name in ("BugSplat64.dll", "echovr_server.exe"):
            keep = section.index(f'--only-keep-debug "${{DIST_STAGE_DIR}}/{name}" "${{DIST_SYMBOLS_DIR}}/{name}.dbg"')
            strip = section.index(f'--strip-all "${{DIST_STAGE_DIR}}/{name}"')
            link = section.index(f'"--add-gnu-debuglink=${{DIST_SYMBOLS_DIR}}/{name}.dbg" "${{DIST_STAGE_DIR}}/{name}"')
            self.assertLess(keep, strip, name)
            self.assertLess(strip, link, name)


class ReleaseWorkflowInstallerSymbolsTest(unittest.TestCase):
    """install-nevr-runtime.exe rides in the Windows zip, so the tree check (verify_symbols.py --tree) needs its symbol file
    in the same symbols directory as the DLLs' (.github/workflows/build.yml)."""

    @staticmethod
    def steps():
        text = (REPO / ".github" / "workflows" / "build.yml").read_text()
        out = {}
        for chunk in text.split("\n    - name: ")[1:]:
            out[chunk.split("\n", 1)[0].strip()] = chunk
        return list(out), out

    def test_the_installer_is_split_stripped_and_linked_into_the_dist_symbols_before_they_are_verified_and_uploaded(self):
        order, steps = self.steps()
        split = "Split, strip and link the core installer's symbols"
        for earlier, later in (("Create distribution packages (unsigned)", split),
                               ("Download the core installer", split),
                               (split, "Verify the symbol files match the stripped binaries"),
                               (split, "Upload the symbol files"),
                               (split, "Gate and assemble the release tree")):
            self.assertLess(order.index(earlier), order.index(later), f"{earlier!r} runs before {later!r}")
        body = steps[split]
        dbg = '"dist/symbols/$name/install-nevr-runtime.exe.dbg"'
        keep = body.index(f'--only-keep-debug "$exe" {dbg}')
        strip = body.index('x86_64-w64-mingw32-strip --strip-all "$exe"')
        link = body.index(f'"--add-gnu-debuglink=dist/symbols/$name/install-nevr-runtime.exe.dbg" "$exe"')
        self.assertLess(keep, strip)
        self.assertLess(strip, link)

    def test_the_installer_is_built_with_line_tables_so_its_symbol_file_carries_dwarf(self):
        # A release build has no DWARF unless asked: verify_symbols.py refuses a symbol file without it.
        self.assertIn("CARGO_PROFILE_RELEASE_DEBUG: line-tables-only", self.steps()[1]["Build the core installer for Windows"])

    def test_the_installer_is_built_once_under_the_new_name_and_copied_once_to_the_old_one(self):
        """#1492: one build, one link step. The old name is a cp of the built file in the packaging step."""
        text = (REPO / ".github" / "workflows" / "build.yml").read_text()
        self.assertEqual(len(re.findall(r"cargo\"? build", text)), 1, "one build of the installer")
        self.assertIn("--bin install-nevr-runtime ", text)
        self.assertIn("release/install-nevr-runtime.exe", text)
        copies = [line.strip() for line in text.splitlines() if re.match(r"\s*cp\b.*nevr-install\.exe", line)]
        self.assertEqual(copies, ['cp "$installer" "$(dirname "$installer")/nevr-install.exe"'])
        assemble = self.steps()[1]["Gate and assemble the release tree"]
        self.assertIn(copies[0], assemble)
        self.assertIn('--installer-exe "$installer"', assemble)
        self.assertIn('--installer-compat-exe "$(dirname "$installer")/nevr-install.exe"', assemble)
        self.assertLess(assemble.index(copies[0]), assemble.index("package_release.py tree"))

    def test_the_symbol_step_says_the_old_name_is_the_same_bytes_checked_through_the_new_name(self):
        body = self.steps()[1]["Verify the symbol files match the stripped binaries"]
        self.assertRegex(body, r"nevr-install\.exe.*same bytes")
        self.assertNotIn("nevr-install.exe.dbg", body)

    def test_the_local_package_recipe_makes_the_old_name_copy_too(self):
        recipe = (REPO / "justfile").read_text().split("package-dev:", 1)[1].split("\n_quest-apk", 1)[0]
        self.assertIn('cp "$installer" "$(dirname "$installer")/nevr-install.exe"', recipe)
        self.assertIn('--installer-compat-exe "$(dirname "$installer")/nevr-install.exe"', recipe)

    def test_the_release_tree_is_checked_for_stripped_binaries_after_the_copy_and_before_the_seal(self):
        """#1492: the copy's byte-identity and its lack of a .dbg are checked by this step, so it must run, on the tree the
        gate assembled (which holds the cp), before that tree is uploaded for the signer and the seal job."""
        order, steps = self.steps()
        gate, check = "Gate and assemble the release tree", "Verify the Windows tree ships only stripped binaries"
        self.assertRegex(steps[check], r'python3 tools/verify_symbols\.py --tree release-stage/nevr-runtime-v\*-windows "dist/symbols/\$name"\n')
        self.assertNotRegex(steps[check], r"(?m)^\s*(true|:|exit 0)\s*$")
        self.assertIn("if: steps.release.outputs.version != ''", steps[check])
        self.assertLess(steps[gate].index('cp "$installer"'), steps[gate].index("package_release.py tree"))
        for earlier, later in ((gate, check), (check, "Upload the release tree"), ("Upload the release tree", "Seal the release")):
            self.assertLess(order.index(earlier), order.index(later), f"{earlier!r} runs before {later!r}")

    def test_the_symbol_check_pairs_the_installer_with_its_symbol_file(self):
        body = self.steps()[1]["Verify the symbol files match the stripped binaries"]
        self.assertRegex(body, r'verify_symbols\.py "\$installer" "dist/symbols/\$name/install-nevr-runtime\.exe\.dbg"')


def make_pe(sections: list[tuple[str, bytes]], nsyms: int = 0) -> bytes:
    """A minimal PE/COFF image: DOS stub pointer, PE signature, COFF header, section table, section data, then
    a COFF symbol table of nsyms 18-byte records and a string table that carries the long section names."""
    strings = b""
    names = []
    for name, _ in sections:
        if len(name) <= 8:
            names.append(name.encode().ljust(8, b"\0"))
        else:
            names.append(b"/" + str(4 + len(strings)).encode().ljust(7, b"\0"))
            strings += name.encode() + b"\0"
    header = 0x40 + 4 + 20 + 40 * len(sections)
    data, table, offset = b"", b"", header
    for (_, payload), raw in zip(sections, names):
        table += struct.pack("<8sIIIIIIHHI", raw, len(payload), 0, len(payload), offset, 0, 0, 0, 0, 0)
        data += payload
        offset += len(payload)
    coff = struct.pack("<HHIIIHH", 0x8664, len(sections), 0, offset, nsyms, 0, 0x2022)
    dos = b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40)
    return (dos + b"PE\0\0" + coff + table + data + b"\x01" * (18 * nsyms)
            + struct.pack("<I", 4 + len(strings)) + strings)


def debuglink(name: str, target: bytes) -> bytes:
    """The .gnu_debuglink payload: NUL-terminated name padded to 4 bytes, then the CRC32 of the symbol file."""
    raw = name.encode() + b"\0"
    return raw.ljust((len(raw) + 3) & ~3, b"\0") + struct.pack("<I", zlib.crc32(target) & 0xFFFFFFFF)


class SyntheticPairTest(unittest.TestCase):
    """verify_symbols against hand-built PEs: every check has a fixture that trips only that check."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="verify-symbols-synth-"))
        self.addCleanup(shutil.rmtree, self.dir, True)

    def pair(self, dbg_sections=None, dbg_nsyms=3, pe_sections=None, pe_nsyms=0, link=None):
        dbg_bytes = make_pe(dbg_sections if dbg_sections is not None else [(".debug_info", b"dwarf")], dbg_nsyms)
        dbg = self.dir / "t.exe.dbg"
        dbg.write_bytes(dbg_bytes)
        sections = pe_sections if pe_sections is not None else [
            (".text", b"code"), (".gnu_debuglink", link if link is not None else debuglink("t.exe.dbg", dbg_bytes))]
        exe = self.dir / "t.exe"
        exe.write_bytes(make_pe(sections, pe_nsyms))
        return exe, dbg

    def assert_one_line(self, result, filename, reason):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        lines = result.stderr.splitlines()
        self.assertEqual(len(lines), 1, result.stderr)
        self.assertTrue(lines[0].startswith(f"verify_symbols: {filename}: "), lines[0])
        self.assertIn(reason, lines[0])

    def test_a_split_pair_with_dwarf_and_function_names_matches(self):
        result = verify(*self.pair())
        self.assertEqual((result.returncode, result.stderr), (0, ""), result.stdout)

    def test_a_symbol_file_without_dwarf_is_refused(self):
        result = verify(*self.pair(dbg_sections=[(".text", b"code")]))
        self.assert_one_line(result, "t.exe.dbg", "carries no DWARF sections")

    def test_a_symbol_file_with_an_empty_coff_symbol_table_is_refused(self):
        result = verify(*self.pair(dbg_nsyms=0))
        self.assert_one_line(result, "t.exe.dbg", "COFF symbol table is empty")

    def test_a_symbol_file_whose_symbol_table_runs_past_the_file_is_refused(self):
        exe, dbg = self.pair(dbg_sections=[(".debug_a", b"dwarf")])  # 8 chars: no string table needed
        blob = bytearray(dbg.read_bytes())
        struct.pack_into("<I", blob, 0x40 + 16, 3000)  # NumberOfSymbols: 3000 declared, 3 present
        dbg.write_bytes(bytes(blob))
        # the debuglink CRC32 is of the file as built; relink so only the symbol table is wrong
        exe.write_bytes(make_pe([(".text", b"code"), (".gnu_debuglink", debuglink("t.exe.dbg", bytes(blob)))]))
        result = verify(exe, dbg)
        self.assert_one_line(result, "t.exe.dbg", "runs past the end of the file")

    def test_a_binary_that_still_carries_dwarf_is_refused(self):
        exe, dbg = self.pair(pe_sections=[(".text", b"code"), (".debug_info", b"dwarf"),
                                          (".gnu_debuglink", debuglink("t.exe.dbg", b""))])
        self.assertIn("still carries DWARF sections", verify(exe, dbg).stderr)

    def test_a_binary_that_still_carries_a_coff_symbol_table_is_refused(self):
        result = verify(*self.pair(pe_nsyms=2))
        self.assert_one_line(result, "t.exe", "still carries a COFF symbol table (2 symbols; not stripped)")

    def test_a_binary_without_a_debuglink_is_refused(self):
        result = verify(*self.pair(pe_sections=[(".text", b"code")]))
        self.assert_one_line(result, "t.exe", "no .gnu_debuglink")

    def test_a_debuglink_with_no_room_for_a_crc_is_refused_not_a_traceback(self):
        for name, link in (("no-nul", b"t.exe.dbg"), ("no-crc", b"t.exe.dbg\0")):
            with self.subTest(link=name):
                self.assert_one_line(verify(*self.pair(link=link)), "t.exe", "too short")

    def test_a_binary_that_is_not_a_pe_is_one_line_not_a_traceback(self):
        exe, dbg = self.pair()
        exe.write_text("#!/bin/sh\necho hi\n")
        self.assert_one_line(verify(exe, dbg), "t.exe", "not a PE file (no MZ header)")

    def test_a_pe_signature_that_is_missing_is_one_line(self):
        exe, dbg = self.pair()
        exe.write_bytes(b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40) + b"XX\0\0" + b"\0" * 64)
        self.assert_one_line(verify(exe, dbg), "t.exe", "no PE signature")

    def test_a_symbol_file_that_is_not_a_pe_is_one_line(self):
        exe, dbg = self.pair()
        dbg.write_bytes(b"\x7fELF" + b"\0" * 60)
        self.assert_one_line(verify(exe, dbg), "t.exe.dbg", "not a PE file")

    def test_a_missing_symbol_file_is_one_line(self):
        exe, dbg = self.pair()
        dbg.unlink()
        self.assert_one_line(verify(exe, dbg), "t.exe.dbg", "cannot read")

    def test_a_missing_binary_is_one_line(self):
        exe, dbg = self.pair()
        exe.unlink()
        self.assert_one_line(verify(exe, dbg), "t.exe", "cannot read")

    def test_a_file_cut_off_inside_a_section_is_one_line(self):
        exe, dbg = self.pair()
        exe.write_bytes(make_pe([(".text", b"code" * 8)])[:0x40 + 4 + 20 + 40 + 10])  # 10 of 32 data bytes
        self.assert_one_line(verify(exe, dbg), "t.exe", "section .text runs")

    def test_a_file_cut_off_inside_the_headers_is_one_line(self):
        exe, dbg = self.pair()
        exe.write_bytes(exe.read_bytes()[:0x40 + 4 + 20 + 40 + 10])  # second section entry is incomplete
        self.assert_one_line(verify(exe, dbg), "t.exe", "truncated file")

    def test_a_tree_with_a_file_that_is_not_a_pe_names_it_and_does_not_traceback(self):
        exe, dbg = self.pair()
        package, symbols = self.dir / "pkg", self.dir / "syms"
        package.mkdir()
        symbols.mkdir()
        (package / "bad.dll").write_text("text, not a PE\n")
        (symbols / "bad.dll.dbg").write_bytes(dbg.read_bytes())
        result = subprocess.run(["python3", "-I", str(TOOL), "--tree", str(package), str(symbols)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIn("verify_symbols: bad.dll: not a PE file (no MZ header)", result.stderr)


@unittest.skipUnless(GCC and OBJCOPY and STRIP, "no mingw toolchain")
class StrippedBinaryMatchesItsSymbolsTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="verify-symbols-"))
        self.addCleanup(shutil.rmtree, self.dir, True)
        (self.dir / "t.c").write_text("int main(void){return 0;}\n")
        subprocess.run([GCC, "-g", "-o", str(self.dir / "full.exe"), str(self.dir / "t.c")], check=True)
        self.exe, self.dbg = self.dir / "t.exe", self.dir / "t.exe.dbg"
        shutil.copy(self.dir / "full.exe", self.exe)
        subprocess.run([OBJCOPY, "--only-keep-debug", str(self.exe), str(self.dbg)], check=True)
        subprocess.run([STRIP, "--strip-all", str(self.exe)], check=True)

    def test_a_release_build_without_dash_g_still_carries_function_names(self):
        """The release preset has no -g: the symbol file is toolchain DWARF plus the COFF symbol table."""
        subprocess.run([GCC, "-O3", "-o", str(self.exe), str(self.dir / "t.c")], check=True)
        subprocess.run([OBJCOPY, "--only-keep-debug", str(self.exe), str(self.dbg)], check=True)
        subprocess.run([STRIP, "--strip-all", str(self.exe)], check=True)
        self.link()
        result = verify(self.exe, self.dbg)
        self.assertEqual(result.returncode, 0, result.stderr)

    def link(self):
        subprocess.run([OBJCOPY, f"--add-gnu-debuglink={self.dbg}", str(self.exe)], check=True)

    def test_a_split_stripped_linked_pair_matches(self):
        self.link()
        result = verify(self.exe, self.dbg)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_symbol_file_that_changed_after_linking_does_not_match(self):
        self.link()
        with self.dbg.open("ab") as f:
            f.write(b"x")
        result = verify(self.exe, self.dbg)
        self.assertEqual(result.returncode, 1)
        self.assertIn("CRC32", result.stderr)

    def test_a_stripped_binary_without_a_debuglink_is_refused(self):
        result = verify(self.exe, self.dbg)
        self.assertEqual(result.returncode, 1)
        self.assertIn("no .gnu_debuglink", result.stderr)

    def test_a_binary_that_still_carries_dwarf_is_refused(self):
        result = verify(self.dir / "full.exe", self.dbg)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not stripped", result.stderr)

    def tree(self, name="pkg"):
        package, symbols = self.dir / name, self.dir / (name + "-symbols")
        package.mkdir()
        symbols.mkdir()
        return package, symbols

    def tree_run(self, package, symbols):
        return subprocess.run(["python3", "-I", str(TOOL), "--tree", str(package), str(symbols)],
                              capture_output=True, text=True)

    def test_a_package_tree_passes_when_every_pe_matches_its_symbol_file(self):
        self.link()
        package, symbols = self.tree()
        shutil.copy(self.exe, package / "t.exe")
        shutil.copy(self.dbg, symbols / "t.exe.dbg")
        (package / "install.ps1").write_text("not a PE\n")
        result = self.tree_run(package, symbols)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_package_tree_with_an_unstripped_pe_is_refused(self):
        package, symbols = self.tree()
        shutil.copy(self.dir / "full.exe", package / "t.exe")
        shutil.copy(self.dbg, symbols / "t.exe.dbg")
        result = self.tree_run(package, symbols)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not stripped", result.stderr)

    def test_a_package_tree_with_a_pe_that_has_no_symbol_file_is_refused(self):
        self.link()
        package, symbols = self.tree()
        shutil.copy(self.exe, package / "t.exe")
        result = self.tree_run(package, symbols)
        self.assertEqual(result.returncode, 1)
        self.assertIn("no symbol file", result.stderr)

    def test_a_windows_tree_with_the_installer_needs_the_installers_symbol_file(self):
        self.link()
        package, symbols = self.tree()
        shutil.copy(self.exe, package / "install-nevr-runtime.exe")
        result = self.tree_run(package, symbols)
        self.assertEqual(result.returncode, 1)
        self.assertIn("install-nevr-runtime.exe: no symbol file", result.stderr)
        shutil.copy(self.dbg, symbols / "install-nevr-runtime.exe.dbg")
        self.assertEqual(self.tree_run(package, symbols).returncode, 1, "a link to t.exe.dbg is not a link to install-nevr-runtime.exe.dbg")

    def test_a_windows_tree_with_the_installer_split_under_its_own_name_passes(self):
        package, symbols = self.tree()
        installer, dbg = package / "install-nevr-runtime.exe", symbols / "install-nevr-runtime.exe.dbg"
        shutil.copy(self.dir / "full.exe", installer)
        subprocess.run([OBJCOPY, "--only-keep-debug", str(installer), str(dbg)], check=True)
        subprocess.run([STRIP, "--strip-all", str(installer)], check=True)
        subprocess.run([OBJCOPY, f"--add-gnu-debuglink={dbg}", str(installer)], check=True)
        result = self.tree_run(package, symbols)
        self.assertEqual(result.returncode, 0, result.stderr)

    def installer_pair(self, package, symbols):
        """install-nevr-runtime.exe split, stripped and linked under its own name, as build.yml does."""
        installer, dbg = package / "install-nevr-runtime.exe", symbols / "install-nevr-runtime.exe.dbg"
        shutil.copy(self.dir / "full.exe", installer)
        subprocess.run([OBJCOPY, "--only-keep-debug", str(installer), str(dbg)], check=True)
        subprocess.run([STRIP, "--strip-all", str(installer)], check=True)
        subprocess.run([OBJCOPY, f"--add-gnu-debuglink={dbg}", str(installer)], check=True)
        return installer

    def test_the_old_name_copy_of_the_installer_needs_no_symbol_file_of_its_own(self):
        """#1492: nevr-install.exe is the same bytes as install-nevr-runtime.exe, so its debuglink names the new
        name's symbol file and the pair check covers it through the new name."""
        package, symbols = self.tree()
        installer = self.installer_pair(package, symbols)
        shutil.copy(installer, package / "nevr-install.exe")
        result = self.tree_run(package, symbols)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((symbols / "nevr-install.exe.dbg").exists())

    def test_the_old_name_copy_with_other_bytes_is_refused(self):
        package, symbols = self.tree()
        installer = self.installer_pair(package, symbols)
        shutil.copy(installer, package / "nevr-install.exe")
        with open(package / "nevr-install.exe", "ab") as f:
            f.write(b"\0one more byte")
        result = self.tree_run(package, symbols)
        self.assertEqual(result.returncode, 1)
        self.assertIn("nevr-install.exe: not the same bytes as install-nevr-runtime.exe", result.stderr)

    def test_the_old_name_copy_without_the_new_name_is_refused(self):
        self.link()
        package, symbols = self.tree()
        shutil.copy(self.exe, package / "nevr-install.exe")
        shutil.copy(self.dbg, symbols / "nevr-install.exe.dbg")
        result = self.tree_run(package, symbols)
        self.assertEqual(result.returncode, 1)
        self.assertIn("nevr-install.exe: install-nevr-runtime.exe is not in the package", result.stderr)

    def test_a_copy_named_like_the_old_installer_in_a_subfolder_is_an_ordinary_pe_and_needs_its_own_symbol_file(self):
        """The exemption is for the package's top level, where the zip holds the copy; elsewhere the name proves nothing."""
        package, symbols = self.tree()
        installer = self.installer_pair(package, symbols)
        (package / "sub").mkdir()
        shutil.copy(installer, package / "sub" / "nevr-install.exe")
        result = self.tree_run(package, symbols)
        self.assertEqual(result.returncode, 1)
        self.assertIn("sub/nevr-install.exe: no symbol file", result.stderr)

    def test_an_empty_package_tree_is_refused(self):
        package, symbols = self.tree()
        self.assertEqual(self.tree_run(package, symbols).returncode, 1)

    def test_a_link_to_another_file_name_is_refused(self):
        self.link()
        other = self.dir / "other.dbg"
        shutil.copy(self.dbg, other)
        result = verify(self.exe, other)
        self.assertEqual(result.returncode, 1)
        self.assertIn("names 't.exe.dbg'", result.stderr)


if __name__ == "__main__":
    unittest.main()
