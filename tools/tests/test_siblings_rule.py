"""AGENTS.md asks a fix PR for the shape it fixed and every sibling instance, and the PR template asks again (#1282)."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def flat(text: str) -> str:
    return " ".join(text.split())


class SiblingsRule(unittest.TestCase):
    def test_agents_md_carries_the_rule_among_what_a_pr_carries(self):
        text = (ROOT / "AGENTS.md").read_text()
        start = text.index("### What a PR carries")
        end = text.index("\n### ", start + 1)
        section = flat(text[start:end])
        self.assertIn("**Siblings, for a fix.**", section)
        for part in ("names the shape it fixed", "the `git grep` that finds every instance of that shape",
                     "fixed in this PR", "filed (`#N`)", "not the same shape (the reason)", "## Siblings"):
            self.assertIn(part, section)
        self.assertLess(section.index("**A search statement.**"), section.index("**Siblings, for a fix.**"))

    def test_the_pr_template_has_the_checklist_item(self):
        template = (ROOT / ".github/pull_request_template.md").read_text()
        checklist = flat(template[template.index("## Checklist"):])
        self.assertRegex(checklist, r"- \[ \] Siblings: a fix names the shape it fixed.*fixed here.*filed.*not the same shape")


if __name__ == "__main__":
    unittest.main()
