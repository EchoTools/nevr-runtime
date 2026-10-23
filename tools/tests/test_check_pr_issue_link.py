"""tools/check_pr_issue_link.py: a PR body names at least one issue it resolves or advances.

Every PR is opened for an existing issue (docs/standards/planning.md, "PRs"), so there is no "no issue"
form. The script is run for real, as a subprocess, once with the body in the environment (as the
workflow does) and once from a file."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check_pr_issue_link.py"


def run(body, via="env"):
    env = {k: v for k, v in os.environ.items() if k != "PR_BODY"}
    if via == "env":
        return subprocess.run([sys.executable, "-I", str(SCRIPT)], capture_output=True, text=True,
                              env={**env, "PR_BODY": body})
    with tempfile.NamedTemporaryFile("w", suffix=".md", encoding="utf-8", newline="") as handle:
        handle.write(body)
        handle.flush()
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--body-file", handle.name],
                              capture_output=True, text=True, env=env)


class AcceptedFormsTest(unittest.TestCase):
    def assertAccepts(self, body, expect=None):
        for via in ("env", "file"):
            result = run(body, via)
            self.assertEqual(result.returncode, 0, f"{via}: {body!r}\n{result.stderr}")
            self.assertTrue(result.stdout.startswith("ok:"), result.stdout)
            self.assertEqual(result.stderr, "")
            if expect:
                self.assertIn(expect, result.stdout)

    def test_each_closing_keyword(self):
        for word in ("Closes", "Fixes", "Resolves", "Close", "Fix", "Resolve", "Closed", "Fixed", "Resolved"):
            self.assertAccepts(f"## Linked issue\n\n{word} #12\n", "#12")

    def test_each_partial_form(self):
        for word in ("Refs", "Ref", "Part of", "Completes"):
            self.assertAccepts(f"{word} #34\n\nStill open: the live run.\n", "#34")

    def test_case_insensitive_and_colon(self):
        self.assertAccepts("closes #5")
        self.assertAccepts("FIXES #5")
        self.assertAccepts("Fixes: #5")

    def test_list_item(self):
        self.assertAccepts("- Closes #7\n* Refs #8\n", "#7")

    def test_issue_in_another_repository(self):
        self.assertAccepts("Closes EchoTools/nakama#684", "EchoTools/nakama#684")

    def test_several_issues_are_all_reported(self):
        result = run("Closes #1\nCloses #2\nRefs #3\n")
        self.assertEqual(result.returncode, 0)
        for n in ("#1", "#2", "#3"):
            self.assertIn(n, result.stdout)

    def test_crlf_body(self):
        self.assertAccepts("What and why\r\n\r\nCloses #830\r\n\r\nRead AGENTS.md at abcdef1\r\n", "#830")

    def test_a_lone_carriage_return_ends_a_line(self):
        self.assertAccepts("What and why\rCloses #9\r", "#9")

    def test_the_reference_may_follow_other_prose(self):
        self.assertAccepts("This changes the check.\n\nIt adds tests.\n\nCloses #830\n")


class RefusedFormsTest(unittest.TestCase):
    def assertRefuses(self, body):
        for via in ("env", "file"):
            result = run(body, via)
            self.assertEqual(result.returncode, 1, f"{via}: {body!r}\n{result.stdout}")
            self.assertEqual(result.stdout, "")
            self.assertTrue(result.stderr.startswith("FAIL:"), result.stderr)
            self.assertEqual(len(result.stderr.strip().splitlines()), 1, result.stderr)

    def test_empty_and_missing_body(self):
        self.assertRefuses("")
        self.assertRefuses("   \n\n")
        result = subprocess.run([sys.executable, "-I", str(SCRIPT)], capture_output=True, text=True,
                                env={k: v for k, v in os.environ.items() if k != "PR_BODY"})
        self.assertEqual(result.returncode, 1)

    def test_prose_without_a_reference(self):
        self.assertRefuses("Fixes the bug in the loader.\n\nRead AGENTS.md at abcdef1\n")

    def test_the_template_placeholder_with_no_number(self):
        self.assertRefuses("## Linked issue\n\nCloses #\n")
        self.assertRefuses("## Linked issue\n\nCloses #<issue>\n")

    def test_a_reference_inside_an_html_comment(self):
        self.assertRefuses("<!-- Closes #12 -->\n")
        self.assertRefuses("<!--\nOne line per issue: Closes #12\n-->\n")
        self.assertRefuses("<!--\nCloses #12\n-->\n")
        self.assertRefuses("What and why\n\n<!--\nRefs #12\nCloses #13\n-->\n")

    def test_a_reference_inside_code(self):
        self.assertRefuses("`Closes #12`\n")
        self.assertRefuses("```\nCloses #12\n```\n")
        self.assertRefuses("~~~\nRefs #12\n~~~\n")

    def test_a_bare_number_or_a_mention(self):
        self.assertRefuses("#12\n")
        self.assertRefuses("See #12 for background.\n")

    def test_a_keyword_that_is_not_the_start_of_a_line(self):
        self.assertRefuses("This does not close #12.\n")
        self.assertRefuses("It never refs #12 at all.\n")

    def test_issue_zero_and_urls(self):
        self.assertRefuses("Closes #0\n")
        self.assertRefuses("Closes https://github.com/EchoTools/nevr-runtime/issues/12\n")

    def test_a_word_that_only_starts_like_a_keyword(self):
        self.assertRefuses("Fixesx #12\n")
        self.assertRefuses("Prefs #12\n")

    def test_the_number_must_end_at_a_word_boundary_and_follow_a_space(self):
        self.assertRefuses("Closes #12abc\n")
        self.assertRefuses("Closes#12\n")

    def test_there_is_no_escape_line(self):
        self.assertRefuses("No issue: a typo.\n")


if __name__ == "__main__":
    unittest.main()
