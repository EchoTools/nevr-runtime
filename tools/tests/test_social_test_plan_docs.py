import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PLAN = ROOT / "docs/design/2026-10-01-social-features-test-plan.md"

# AGENTS.md "Documentation": docs state the current tree, with no commit shas or dated narrative.
SHA = re.compile(r"\b(?=[0-9a-f]*[a-f])(?=[0-9a-f]*\d)[0-9a-f]{7,10}\b")
RUN_ID = re.compile(r"\b20\d{6}T\d{6}\b")
DATE = re.compile(r"\b20\d{2}-\d{2}-\d{2}\b")
BUILD_DESCRIBE = re.compile(r"\bv\d+\.\d+\.\d+-\d+-g[0-9a-f]{7,}\b")


class SocialTestPlanDocs(unittest.TestCase):
    def test_no_shas_run_ids_or_dates_outside_file_names(self):
        # The plan's own file names carry a date (docs/design/2026-10-01-...); those are paths, not narrative.
        text = re.sub(r"docs/design/\d{4}-\d{2}-\d{2}-[\w-]+\.md", "", PLAN.read_text())
        found = {}
        for label, pattern in (("sha", SHA), ("run id", RUN_ID), ("date", DATE), ("build", BUILD_DESCRIBE)):
            hits = pattern.findall(text)
            if hits:
                found[label] = hits[:5]
        self.assertEqual(found, {}, "dated narrative or commit references in the social test plan")


if __name__ == "__main__":
    unittest.main()
