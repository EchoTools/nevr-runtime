"""A stripped PE and its split-off symbol file match by .gnu_debuglink name and CRC32 (tools/verify_symbols.py).

The fixture is a tiny mingw executable put through the same objcopy/strip sequence the dist target in
CMakeLists.txt runs; the test skips when no mingw compiler is installed."""
from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
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
