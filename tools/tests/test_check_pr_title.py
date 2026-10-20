"""tools/check_pr_title.py: a PR title is a Conventional Commits 1.0.0 header.

Run for real, as a subprocess, on sample titles. The spec: https://www.conventionalcommits.org/en/v1.0.0/"""
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check_pr_title.py"


def run(title):
    return subprocess.run([sys.executable, "-I", str(SCRIPT), "--title", title], capture_output=True, text=True)


class CheckPrTitleTest(unittest.TestCase):
    def assertPasses(self, title):
        result = run(title)
        self.assertEqual(result.returncode, 0, f"{title!r}: {result.stderr}")
        self.assertTrue(result.stdout.startswith("ok:"), result.stdout)
        self.assertEqual(result.stderr, "")

    def assertFails(self, title):
        result = run(title)
        self.assertEqual(result.returncode, 1, f"{title!r} passed: {result.stdout}")
        self.assertTrue(result.stderr.startswith("FAIL:"), result.stderr)
        self.assertEqual(len(result.stderr.strip().splitlines()), 1, "one FAIL line")

    def test_a_type_a_scope_and_a_description_pass(self):
        self.assertPasses("fix(boot): accept --server as a server flag")
        self.assertPasses("feat(cli): kebab-case flag names (#697)")

    def test_the_scope_is_optional(self):
        self.assertPasses("docs: name the public specs")

    def test_a_bang_marks_a_breaking_change_with_or_without_a_scope(self):
        self.assertPasses("feat!: drop the old flag names")
        self.assertPasses("feat(cli)!: drop the old flag names")

    def test_the_type_is_case_insensitive_per_spec_item_15(self):
        self.assertPasses("Fix(boot): accept --server")
        self.assertPasses("FEAT: add a thing")

    def test_every_listed_type_passes(self):
        for kind in ("feat", "fix", "docs", "style", "refactor", "perf", "test", "build", "ci", "chore", "revert"):
            self.assertPasses(f"{kind}: something")

    def test_a_title_with_no_type_fails(self):
        self.assertFails("accept --server as a server flag")
        self.assertFails("(boot): accept --server")

    def test_an_unknown_type_fails(self):
        self.assertFails("wip(boot): accept --server")

    def test_a_missing_colon_or_space_fails(self):
        self.assertFails("fix(boot) accept --server")
        self.assertFails("fix(boot):accept --server")
        self.assertFails("fix boot: accept --server")

    def test_an_empty_description_fails(self):
        self.assertFails("fix(boot):")
        self.assertFails("fix(boot): ")

    def test_an_empty_or_malformed_scope_fails(self):
        self.assertFails("fix(): accept --server")
        self.assertFails("fix(boot: accept --server")
        self.assertFails("fix(a)(b): accept --server")

    def test_a_bang_in_the_wrong_place_fails(self):
        self.assertFails("fix!(boot): accept --server")

    def test_a_multi_line_title_fails(self):
        self.assertFails("fix: one\nfix: two")
        self.assertFails("fix: one\n")

    def test_no_capital_rule_the_description_may_start_lower_or_upper(self):
        self.assertPasses("fix(boot): Accept --server")
        self.assertPasses("fix(boot): accept --server")

    def test_the_title_may_come_from_the_environment(self):
        result = subprocess.run([sys.executable, "-I", str(SCRIPT)], capture_output=True, text=True,
                                env={"PR_TITLE": "fix: ok"})
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run([sys.executable, "-I", str(SCRIPT)], capture_output=True, text=True,
                                env={"PR_TITLE": "nonsense"})
        self.assertEqual(result.returncode, 1)

    def test_no_title_at_all_fails(self):
        result = subprocess.run([sys.executable, "-I", str(SCRIPT)], capture_output=True, text=True, env={})
        self.assertEqual(result.returncode, 1)
        self.assertTrue(result.stderr.startswith("FAIL:"))


if __name__ == "__main__":
    unittest.main()
