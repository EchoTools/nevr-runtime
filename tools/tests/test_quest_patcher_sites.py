"""tools/quest_patcher_sites.py (#944): the patch repo's libpnsovr.so sites mapped to our hooks.

The real table must pass on its own; hand-made tables show each rule fails; a hand-built AArch64 ELF (the real
library is not in the repository) shows the library check reads the stock word and the function.
"""

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "quest_patcher_sites.py"
sys.path.insert(0, str(REPO / "tools"))
import quest_patcher_sites as sites  # noqa: E402

_spec = importlib.util.spec_from_file_location("walk_fixture", REPO / "tools" / "tests" / "test_pinned_ovr_import_walk.py")
walk_fixture = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(walk_fixture)

HOOKED = "src/quest/login/login_prerequisites.h:OnMessageIsError"


def row(vaddr: int = 0x1ece7c, stock: str = "36000440", function: str = "Fn", covered: str = HOOKED,
        reason: str = "why") -> str:
    return "\t".join(["file.yaml:1", f"{vaddr + 0x100000:#x}", f"{vaddr:#x}", stock, function, "nop", covered, reason])


class TableTest(unittest.TestCase):
    def table(self, *lines: str) -> Path:
        directory = Path(tempfile.mkdtemp(prefix="patcher-sites-"))
        self.addCleanup(lambda: [p.unlink() for p in directory.iterdir()] or directory.rmdir())
        path = directory / "sites.tsv"
        path.write_text("# header\n" + "".join(line + "\n" for line in lines), encoding="utf-8")
        return path

    def failures(self, *lines: str) -> list:
        return sites.check_rows(sites.read_table(self.table(*lines)), REPO)

    def test_the_real_table_passes(self):
        result = subprocess.run([sys.executable, str(TOOL)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("OK 79 rows", result.stdout)

    def test_the_real_table_has_every_patch_repo_site(self):
        rows = sites.read_table(sites.TABLE)
        sources = [r["source"] for r in rows]
        self.assertEqual(sum(s.startswith("libpnsovr.so.yaml:") for s in sources), 8)
        self.assertEqual(sum(s.startswith("license_patch.yaml:") and s != "license_patch.yaml:13" for s in sources), 70)
        self.assertIn("license_patch.yaml:13", sources)  # the second per-player id slot

    def test_a_row_with_a_hook_or_a_reason_passes(self):
        self.assertEqual(self.failures(row(), row(0x1ecf0c, covered="-")), [])
        self.assertEqual(self.failures(row(0x1ecf0c, reason="-")), [])

    def test_a_row_with_neither_a_hook_nor_a_reason_fails(self):
        failures = self.failures(row(covered="-", reason="-"))
        self.assertEqual(len(failures), 1)
        self.assertIn("neither a hook nor a reason", failures[0])
        self.assertTrue(self.failures(row(covered="", reason="")))

    def test_a_hook_that_is_not_in_the_tree_fails(self):
        self.assertIn("not found", self.failures(row(covered="src/quest/login/login_prerequisites.h:NoSuchHook"))[0])
        self.assertIn("existing", self.failures(row(covered="src/quest/no_such_file.h:OnMessageIsError"))[0])
        self.assertIn("existing", self.failures(row(covered="src/quest/login/login_prerequisites.h"))[0])
        # A symbol that is only part of a longer name does not count.
        self.assertIn("not found", self.failures(row(covered="src/quest/login/login_prerequisites.h:OnMessageIs"))[0])

    def test_every_hook_in_a_list_is_checked(self):
        both = HOOKED + ", src/quest/login/login_prerequisites.h:Missing"
        self.assertEqual(len(self.failures(row(covered=both))), 1)

    def test_the_vaddr_must_be_their_address_less_the_disasm_offset(self):
        bad = row().split("\t")
        bad[1] = "0x2ece80"
        self.assertIn("is not their_va", self.failures("\t".join(bad))[0])

    def test_a_vaddr_listed_twice_fails(self):
        self.assertIn("already at line", self.failures(row(), row())[0])

    def test_a_short_row_fails(self):
        self.assertIn("fields", self.failures("file.yaml:1\t0x2ece7c")[0])

    def test_the_pinned_recipe_checks_the_library(self):
        justfile = (REPO / "justfile").read_text()
        start = justfile.index("\ntest-quest-hooks-pinned ")
        end = justfile.index("\n\n", start)
        body = [line.strip() for line in justfile[start:end].splitlines() if not line.strip().startswith("#")]
        self.assertIn('python3 tools/quest_patcher_sites.py --lib "$out/lib/lib/arm64-v8a/libpnsovr.so"', body)

    def test_an_empty_change_fails(self):
        bad = row().split("\t")
        bad[5] = "-"
        self.assertIn("empty change", self.failures("\t".join(bad))[0])


class LibraryTest(unittest.TestCase):
    def library(self) -> Path:
        directory = Path(tempfile.mkdtemp(prefix="patcher-sites-lib-"))
        self.addCleanup(lambda: [p.unlink() for p in directory.iterdir()] or directory.rmdir())
        path = directory / "lib.so"
        words = [0xD503201F, walk_fixture.RET]  # nop; ret
        path.write_bytes(walk_fixture.build(["ovr_A"], words, (("Holder", walk_fixture.TEXT, 8),)))
        return path

    def rows(self, *lines: str) -> list:
        directory = Path(tempfile.mkdtemp(prefix="patcher-sites-"))
        self.addCleanup(lambda: [p.unlink() for p in directory.iterdir()] or directory.rmdir())
        path = directory / "sites.tsv"
        path.write_text("".join(line + "\n" for line in lines), encoding="utf-8")
        return sites.read_table(path)

    def test_the_stock_word_and_function_match(self):
        lib = self.library()
        text = walk_fixture.TEXT
        stub = walk_fixture.PLT + 32
        good = self.rows(row(text, "d503201f", "Holder"), row(text + 4, "d65f03c0", "Holder"),
                         row(stub, f"{walk_fixture.stub(0)[0]:08x}", "ovr_A@plt"))
        self.assertEqual(sites.check_library(good, lib), [])

    def test_a_changed_word_fails(self):
        failures = sites.check_library(self.rows(row(walk_fixture.TEXT, "d65f03c0", "Holder")), self.library())
        self.assertEqual(len(failures), 1)
        self.assertIn("holds d503201f", failures[0])

    def test_a_wrong_function_fails(self):
        failures = sites.check_library(self.rows(row(walk_fixture.TEXT, "d503201f", "Other")), self.library())
        self.assertIn("is in Holder", failures[0])

    def test_an_address_outside_the_library_fails(self):
        failures = sites.check_library(self.rows(row(0x900000, "d503201f", "Holder")), self.library())
        self.assertIn("not in the library", failures[0])


if __name__ == "__main__":
    unittest.main()
