"""A state the design calls wrong is marked `KNOWN-WRONG #<issue>`, and no test pins a fix's absence (#1302).

Each case builds a small git tree, runs tools/check_known_wrong.py over it as a process, and reads its exit code and
its lines; the tree itself must pass.
"""
import os
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "check_known_wrong.py"


def run(root, *args, env=None):
    return subprocess.run([sys.executable, str(TOOL), "--root", str(root), *args], capture_output=True, text=True,
                          env=env)


class Tree:
    def __init__(self, files):
        self._dir = tempfile.TemporaryDirectory()
        self.path = Path(self._dir.name)
        for rel, text in files.items():
            target = self.path / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(textwrap.dedent(text), encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(self.path)], check=True)
        subprocess.run(["git", "-C", str(self.path), "add", "-A"], check=True)

    def __enter__(self):
        return self.path

    def __exit__(self, *exc):
        self._dir.cleanup()


TEST_HEADER = "import unittest\n\nclass T(unittest.TestCase):\n    def test_x(self):\n        code = ''\n"


class KnownWrong(unittest.TestCase):
    def assertFails(self, files, needle, *args):
        with Tree(files) as root:
            result = run(root, *args)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(needle, result.stderr)
        self.assertIn("known-wrong: FAIL — ", result.stderr)
        return result

    def assertPasses(self, files, *args, env=None):
        with Tree(files) as root:
            result = run(root, *args, env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("known-wrong OK", result.stdout)
        return result

    def test_the_tree_passes(self):
        result = run(ROOT)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("known-wrong OK", result.stdout)

    def test_the_default_root_is_this_checkout(self):
        result = subprocess.run([sys.executable, str(TOOL)], capture_output=True, text=True, cwd=tempfile.gettempdir())
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_an_assert_not_in_whose_message_defers_a_fix_fails(self):
        test = TEST_HEADER + "        self.assertNotIn('nevr_root', code, 'where config.yaml lives is ADR 0012 step 6')\n"
        result = self.assertFails({"tools/tests/test_a.py": test}, "tools/tests/test_a.py:6: assertNotIn pins")
        self.assertIn('"ADR 0012 step 6"', result.stderr)

    def test_an_assert_not_in_with_a_keyword_message_that_defers_fails(self):
        test = TEST_HEADER + "        self.assertNotIn('x', code, msg=f'{code} is not yet moved')\n"
        self.assertFails({"tools/tests/test_a.py": test}, "tools/tests/test_a.py:6:")

    def test_a_deferring_comment_right_above_an_assert_not_in_fails(self):
        test = TEST_HEADER + "        # the fix comes until the move lands\n        self.assertNotIn('x', code)\n"
        self.assertFails({"tools/tests/test_a.py": test}, "tools/tests/test_a.py:7:")

    def test_a_comment_separated_by_code_does_not_count(self):
        test = TEST_HEADER + "        # not yet\n        y = 1\n        self.assertNotIn('x', code)\n"
        self.assertPasses({"tools/tests/test_a.py": test})

    def test_a_regression_guard_that_cites_an_issue_passes(self):
        test = TEST_HEADER + "        self.assertNotIn('ctx', code, 'the game function takes the net game (#730)')\n"
        self.assertPasses({"tools/tests/test_a.py": test})

    def test_only_tools_tests_test_files_are_read_for_assertions(self):
        body = TEST_HEADER + "        self.assertNotIn('x', code, 'not yet')\n"
        self.assertPasses({"tools/tests/helper.py": body, "tests/test_a.py": body})

    def test_a_doc_that_defers_a_build_without_an_issue_fails(self):
        self.assertFails({"docs/a.md": "# A\n\nThe uploader is\nnot yet implemented.\n"},
                         'docs/a.md:3: "not yet implemented" defers a build and names no issue')

    def test_until_it_lands_without_an_issue_fails(self):
        self.assertFails({"docs/a.md": "The old path stays until the new route lands.\n"}, "docs/a.md:1:")

    def test_a_deferral_that_names_its_issue_passes(self):
        self.assertPasses({"docs/a.md": "The uploader is not yet implemented (#1303).\n"})

    def test_the_issue_must_be_in_the_same_paragraph(self):
        self.assertFails({"docs/a.md": "Tracked by #12.\n\nThe uploader is not yet implemented.\n"}, "docs/a.md:3:")

    def test_a_list_item_and_a_table_row_are_their_own_units(self):
        self.assertFails({"docs/a.md": "Issues: #4.\n- the plugins are not yet moved\n"}, "docs/a.md:2:")
        self.assertFails({"docs/a.md": "| #4 | ok |\n| x | not yet wired |\n"}, "docs/a.md:2:")

    def test_a_line_that_starts_with_an_issue_is_not_a_heading(self):
        self.assertPasses({"docs/a.md": "The gate holds until #943,\n#944 and #945 are closed; nothing lands until each issue lands.\n"})

    def test_a_heading_is_its_own_unit(self):
        self.assertFails({"docs/a.md": "## Not yet implemented\nSee #5.\n"}, "docs/a.md:1:")

    def test_a_deferral_inside_a_code_fence_passes(self):
        self.assertPasses({"docs/a.md": "```\n\nnot yet implemented\n\n```\n"})

    def test_a_phrase_quoted_in_backticks_is_named_not_said(self):
        self.assertPasses({"docs/a.md": "A doc that says `not\nyet implemented` names `KNOWN-WRONG #<issue>`.\n"})
        self.assertFails({"src/a.h": "// `KNOWN-WRONG` here\n"}, "src/a.h:1:")

    def test_the_audit_record_is_exempt(self):
        self.assertPasses({"docs/audits/2026-01-01.md": "not yet implemented\n"})

    def test_a_smoke_row_that_passes_on_expect_none_fails(self):
        row = "| [ ] | D11 | flags | PC | Start with `-timestamps`. Expect none | line |\n"
        self.assertFails({"docs/testing/smoke.md": row}, 'docs/testing/smoke.md:1: a row passes on "Expect none"')

    def test_a_marked_expect_none_row_passes(self):
        self.assertPasses({"docs/testing/smoke.md": "| D11 | KNOWN-WRONG #1279: Expect none |\n"})

    def test_a_marker_without_an_issue_fails(self):
        result = self.assertFails({"src/a.cpp": "// KNOWN-WRONG: the server login is masked\n"},
                                  "src/a.cpp:1: KNOWN-WRONG names no issue")
        self.assertNotIn("KNOWN-WRONG #", result.stderr.split("FAIL — ")[1].split("(")[0])

    def test_a_marker_with_a_space_but_no_number_fails(self):
        self.assertFails({"justfile": "# KNOWN-WRONG #x\n"}, "justfile:1:")

    def test_a_marker_with_an_issue_passes(self):
        self.assertPasses({"src/a.cpp": "// KNOWN-WRONG #1255: config.yaml is not read from the root\n"})

    def _fake_gh(self, directory, states):
        script = Path(directory) / "gh"
        cases = "".join(f'  {n}) echo {s};;\n' for n, s in states.items())
        script.write_text(f'#!/bin/sh\ncase "$3" in\n{cases}  *) echo "no issue $3" >&2; exit 1;;\nesac\n')
        script.chmod(script.stat().st_mode | stat.S_IEXEC)
        return dict(os.environ, GH=str(script))

    def test_online_a_marker_that_names_a_closed_issue_fails(self):
        with tempfile.TemporaryDirectory() as bin_dir, Tree({"a.md": "KNOWN-WRONG #7 and KNOWN-WRONG #8\n"}) as root:
            result = run(root, "--online", env=self._fake_gh(bin_dir, {7: "OPEN", 8: "CLOSED"}))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("#8: a KNOWN-WRONG marker names an issue that is CLOSED", result.stderr)
        self.assertNotIn("#7:", result.stderr)

    def test_online_an_issue_gh_cannot_read_fails(self):
        with tempfile.TemporaryDirectory() as bin_dir, Tree({"a.md": "KNOWN-WRONG #9\n"}) as root:
            result = run(root, "--online", env=self._fake_gh(bin_dir, {}))
        self.assertEqual(result.returncode, 1)
        self.assertIn("#9: gh could not read the issue: no issue 9", result.stderr)

    def test_online_open_markers_pass(self):
        with tempfile.TemporaryDirectory() as bin_dir:
            result = self.assertPasses({"a.md": "KNOWN-WRONG #7\n"}, "--online",
                                       env=self._fake_gh(bin_dir, {7: "OPEN"}))
        self.assertIn("(all open)", result.stdout)

    def test_offline_does_not_ask_github(self):
        with tempfile.TemporaryDirectory() as bin_dir:
            self.assertPasses({"a.md": "KNOWN-WRONG #8\n"}, env=self._fake_gh(bin_dir, {8: "CLOSED"}))

    def test_a_tree_git_cannot_list_exits_2(self):
        with tempfile.TemporaryDirectory() as plain:
            result = run(plain)
        self.assertEqual(result.returncode, 2)
        self.assertIn("known-wrong: cannot read the tree", result.stderr)

    def test_agents_md_states_the_rule(self):
        text = " ".join((ROOT / "AGENTS.md").read_text(encoding="utf-8").split())
        start = text.index("### Documentation, plans and findings")
        section = text[start:text.index("### ", start + 4)]
        for part in ("**A known-wrong state is marked, never written as the spec.**", "`KNOWN-WRONG #<open issue>`",
                     "No test asserts the absence of a fix", "\"Expect none\"", "names the issue that tracks it",
                     "`tools/check_known_wrong.py`", "`--online`"):
            self.assertIn(part, section)

    def test_the_wiring_test_no_longer_pins_config_yaml_out_of_the_root(self):
        text = (ROOT / "tools/tests/test_nevr_root_wiring.py").read_text(encoding="utf-8")
        self.assertNotIn("ADR 0012 step 6", text)
        self.assertIn("KNOWN-WRONG #1255", text)


if __name__ == "__main__":
    unittest.main()
