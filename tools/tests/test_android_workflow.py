"""The Android workflow runs only on demand (workflow_dispatch), and builds with the pinned NDK and vcpkg."""
from __future__ import annotations

import pathlib
import unittest

import yaml

REPO = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github/workflows/android.yml"


class AndroidWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.workflow = yaml.safe_load(WORKFLOW.read_text())
        self.triggers = self.workflow.get("on", self.workflow.get(True))

    def test_the_workflow_runs_only_on_demand(self):
        self.assertEqual(set(self.triggers), {"workflow_dispatch"})

    def test_the_job_builds_and_tests_with_the_pinned_ndk_and_vcpkg(self):
        text = WORKFLOW.read_text()
        self.assertIn("just test-android", text)
        self.assertIn("26.3.11579264", text)
        self.assertIn('.vcpkg-commit', text)
        ndk = (REPO / "src/quest/CMakePresets.json").read_text()
        self.assertIn("NDK r26d", ndk)


if __name__ == "__main__":
    unittest.main()
