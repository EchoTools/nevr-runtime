"""The N-rules index says what kind of claim each rule is and how well it is checked (#1283).

A rule about what a run does (it exits, logs, refuses, never blocks) is checked by something that drives that
behaviour, rank 1 or 2 on the ladder in docs/standards/verification.md, or its "Checked by" cell says `unchecked:`.
A sensor that finds a call site (rank 3) proves the source has a shape, not that the run behaves.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = "| Rule | Kind | Statement | Guards | Checked by |"
KINDS = {"run", "source"}
ITEM = re.compile(r"^(rank [1235]: |unchecked: |none\b)")


def index_section(text: str) -> str:
    start = text.index("### The N-rules index")
    end = text.index("\n## ", start)
    return text[start:end]


def table_headers(section: str) -> list[str]:
    """The header line of every table in the index: the line above each `| ---` separator row."""
    lines = section.splitlines()
    return [lines[i - 1] for i, line in enumerate(lines) if line.startswith("| ---") and i > 0]


def rows(section: str) -> list[list[str]]:
    """Every row of every table in the index, as its cells. A table is whatever sits under a `| ---` separator, so a
    table whose header was renamed is still parsed (and its header fails test_every_table_has_the_kind_column).
    A `|` escaped as `\\|` stays inside its cell."""
    found = []
    in_table = False
    for line in section.splitlines():
        if line.startswith("| ---"):
            in_table = True
            continue
        if not in_table:
            continue
        if not line.startswith("|"):
            in_table = False
            continue
        cells = [c.strip().replace("\\|", "|") for c in re.split(r"(?<!\\)\|", line.strip())[1:-1]]
        found.append(cells)
    return found


def items(cell: str) -> list[str]:
    """The `;`-separated items of a Checked-by cell, ignoring a `;` inside backticks or parentheses."""
    parts, depth, tick, current = [], 0, False, ""
    for ch in cell:
        if ch == "`":
            tick = not tick
        elif not tick and ch == "(":
            depth += 1
        elif not tick and ch == ")":
            depth -= 1
        if ch == ";" and depth == 0 and not tick:
            parts.append(current.strip())
            current = ""
        else:
            current += ch
    parts.append(current.strip())
    return [p for p in parts if p]


class NRulesIndex(unittest.TestCase):
    def setUp(self):
        self.section = index_section((ROOT / "AGENTS.md").read_text(encoding="utf-8"))
        self.rows = rows(self.section)

    def test_every_table_has_the_kind_column(self):
        headers = table_headers(self.section)
        self.assertGreater(len(headers), 0)
        for header in headers:
            self.assertEqual(header, HEADER)

    def test_the_header_states_the_rule(self):
        flat = " ".join(self.section.split())
        for part in ("A rule whose Kind is `run`", "rank 1 or rank 2", "`unchecked:`", "A `source` rule"):
            self.assertIn(part, flat)

    def test_every_row_has_a_kind_and_every_check_its_rank(self):
        self.assertGreater(len(self.rows), 50)
        for cells in self.rows:
            with self.subTest(rule=cells[0]):
                self.assertEqual(len(cells), 5, cells)
                self.assertIn(cells[1], KINDS)
                checks = items(cells[4])
                self.assertTrue(checks, "Checked by is empty: say how it is checked, or `none`")
                for item in checks:
                    self.assertRegex(item, ITEM)

    def test_a_run_rule_is_driven_or_says_unchecked(self):
        for cells in self.rows:
            if cells[1] != "run":
                continue
            with self.subTest(rule=cells[0]):
                checks = items(cells[4])
                self.assertTrue(any(re.match(r"rank [12]: |unchecked: ", c) for c in checks), cells[4])

    def test_none_is_only_for_a_source_rule(self):
        for cells in self.rows:
            with self.subTest(rule=cells[0]):
                if any(c.startswith("none") for c in items(cells[4])):
                    self.assertEqual(cells[1], "source")

    def test_n1_is_unchecked_because_its_test_runs_a_copy(self):
        n1 = next(c for c in self.rows if c[0] == "N1")
        self.assertTrue(n1[4].startswith("unchecked: "), n1[4])
        self.assertIn("copy", n1[4])

    def test_n147_cites_the_tests_that_hold_the_late_load_cases(self):
        n147 = next(c for c in self.rows if c[0] == "N147")
        self.assertNotIn("`tools/tests/test_build_identity_literal.py` (the late-load", n147[4])
        for name in ("PluginLateLoadTest", "PluginDllMainLoadTest", "ALateLoadUnderANonAsciiGamePathIsRefused"):
            self.assertIn(name, n147[4])


if __name__ == "__main__":
    unittest.main()
