"""AGENTS.md asks a PR that changes player-facing behavior for the other platform's status, and the PR template asks again (#942)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def flat(text: str) -> str:
    return " ".join(text.split())


class PlatformParityRule(unittest.TestCase):
    def test_agents_md_carries_the_rule_among_what_a_pr_carries(self):
        text = (ROOT / "AGENTS.md").read_text()
        start = text.index("### What a PR carries")
        end = text.index("\n### ", start + 1)
        section = flat(text[start:end])
        self.assertIn("**Platform parity.** PCVR and Quest stay at parity.", section)
        for status in ("done in this PR", "already present (`file:line`)", "not applicable (the reason)",
                       "a linked twin issue filed in the same turn"):
            self.assertIn(status, section)

    def test_the_pr_template_has_the_checklist_item_and_keeps_what_the_workflow_tests_pin(self):
        template = (ROOT / ".github/pull_request_template.md").read_text()
        checklist = template[template.index("## Checklist"):]
        self.assertRegex(flat(checklist), r"- \[ \] Platform parity: .*done in this PR.*already present.*not applicable.*twin issue")
        self.assertEqual([l for l in template.splitlines() if l.strip()][-1], "Read AGENTS.md at <sha>")
        self.assertIn("- [ ] The quick check passes", template)


if __name__ == "__main__":
    unittest.main()
