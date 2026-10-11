"""Every external action is pinned by full commit SHA, and none is on a major that still declares the
retired Node 20 runtime (#330).

The pin carries its major in a trailing `# vN` comment. The minimums are the first major of each action
whose action.yml says `runs.using: node24`."""
from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github/workflows"

# action -> first major on Node 24 (checked against each release's action.yml)
MIN_MAJOR = {
    "actions/checkout": 6,
    "actions/cache": 5,
    "actions/cache/restore": 5,
    "actions/cache/save": 5,
    "actions/upload-artifact": 6,
    "actions/download-artifact": 7,
    "softprops/action-gh-release": 3,
}


USES = re.compile(r"\buses:\s*(?P<action>[\w./-]+)@(?P<ref>\S+)(?:\s+#\s*v(?P<major>\d+))?")


def uses_lines():
    """Every external `uses:` in a workflow or a composite action: (file, line number, action, ref, major)."""
    files = sorted(WORKFLOWS.glob("*.yml")) + sorted((REPO / ".github/actions").glob("*/action.yml"))
    for workflow in files:
        for number, line in enumerate(workflow.read_text().splitlines(), 1):
            match = USES.search(line)
            if match:
                major = match.group("major")
                yield workflow.name, number, match.group("action"), match.group("ref"), int(major) if major else None


def references():
    for name, number, action, _, major in uses_lines():
        if major is not None:
            yield name, number, action, major


class WorkflowActionRuntimeTest(unittest.TestCase):
    def test_the_sensor_sees_the_actions(self):
        seen = {action for _, _, action, _ in references()}
        self.assertIn("actions/checkout", seen)
        self.assertIn("actions/cache/restore", seen)

    def test_every_external_action_is_pinned_by_full_commit_sha_with_its_major(self):
        count = 0
        for name, number, action, ref, major in uses_lines():
            count += 1
            self.assertRegex(ref, r"^[0-9a-f]{40}$", f"{name}:{number} {action}@{ref} is not a full commit SHA")
            self.assertIsNotNone(major, f"{name}:{number} {action} has no `# vN` comment naming its major")
        self.assertGreaterEqual(count, 30, "the sensor saw too few external uses")

    def test_no_action_is_on_a_node20_major(self):
        for name, number, action, major in references():
            floor = MIN_MAJOR.get(action)
            if floor is not None:
                self.assertGreaterEqual(major, floor, f"{name}:{number} {action}@v{major} declares node20")


if __name__ == "__main__":
    unittest.main()
