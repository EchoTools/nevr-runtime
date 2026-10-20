"""The PR title workflow (pr-title-check.yml): its trust boundary and its wiring, pinned by contract."""
import re
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "pr-title-check.yml"


def load():
    data = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    return data, data.get("on", data.get(True))  # PyYAML reads the bare key `on` as True


class PrTitleCheckWorkflowTest(unittest.TestCase):
    def test_it_fires_on_pull_request_only_and_when_the_title_is_edited(self):
        _, triggers = load()
        self.assertEqual(list(triggers), ["pull_request"])
        self.assertEqual(sorted(triggers["pull_request"]["types"]), ["edited", "opened", "reopened", "synchronize"])

    def test_it_is_never_pull_request_target(self):
        code = "\n".join(line for line in WORKFLOW.read_text(encoding="utf-8").splitlines()
                         if not line.lstrip().startswith("#"))
        self.assertNotIn("pull_request_target", code)

    def test_permissions_are_contents_read_and_no_secret_is_named(self):
        data, _ = load()
        self.assertEqual(data["permissions"], {"contents": "read"})
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(re.findall(r"secrets\.\w+|secrets\[|secrets:\s*inherit", text), [])

    def test_the_title_reaches_the_script_through_env_only(self):
        data, _ = load()
        steps = [s for job in data["jobs"].values() for s in job["steps"] if "run" in s]
        self.assertEqual(len(steps), 1)
        self.assertEqual(steps[0]["env"], {"PR_TITLE": "${{ github.event.pull_request.title }}"})
        self.assertNotIn("${{", steps[0]["run"])
        self.assertEqual(steps[0]["run"].strip(), "python3 tools/check_pr_title.py")
        self.assertTrue((REPO / "tools" / "check_pr_title.py").is_file())


if __name__ == "__main__":
    unittest.main()
