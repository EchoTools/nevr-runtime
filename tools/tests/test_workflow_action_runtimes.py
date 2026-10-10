"""No workflow uses an action major that still declares the retired Node 20 runtime (#330).

The minimums are the first major of each action whose action.yml says `runs.using: node24`."""
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


def references():
    for workflow in sorted(WORKFLOWS.glob("*.yml")):
        for number, line in enumerate(workflow.read_text().splitlines(), 1):
            match = re.search(r"\buses:\s*([\w./-]+)@v(\d+)", line)
            if match:
                yield workflow.name, number, match.group(1), int(match.group(2))


class WorkflowActionRuntimeTest(unittest.TestCase):
    def test_the_sensor_sees_the_actions(self):
        seen = {action for _, _, action, _ in references()}
        self.assertIn("actions/checkout", seen)
        self.assertIn("actions/cache/restore", seen)

    def test_no_action_is_on_a_node20_major(self):
        for name, number, action, major in references():
            floor = MIN_MAJOR.get(action)
            if floor is not None:
                self.assertGreaterEqual(major, floor, f"{name}:{number} {action}@v{major} declares node20")


if __name__ == "__main__":
    unittest.main()
