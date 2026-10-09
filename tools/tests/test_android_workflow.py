"""The Android workflow's path filter covers every shared source src/quest compiles, so a change to one
of them runs the Quest build before it merges."""
from __future__ import annotations

import fnmatch
import pathlib
import re
import unittest

import yaml

REPO = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github/workflows/android.yml"


def matches(path, pattern):
    # GitHub's ** crosses directories; fnmatch's * already does, so ** needs no special case.
    return fnmatch.fnmatchcase(path, pattern.replace("**", "*"))


class AndroidWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.workflow = yaml.safe_load(WORKFLOW.read_text())
        self.triggers = self.workflow.get("on", self.workflow.get(True))

    def test_pull_request_and_main_push_use_the_same_paths(self):
        self.assertEqual(self.triggers["pull_request"]["paths"], self.triggers["push"]["paths"])

    def test_every_runtime_source_the_quest_cmake_compiles_is_covered(self):
        cmake = (REPO / "src/quest/CMakeLists.txt").read_text()
        sources = sorted(set(re.findall(r"\.\./(runtime/[\w/.-]+\.(?:cpp|c|h))", cmake)))
        self.assertGreaterEqual(len(sources), 3, "the sensor found no shared sources in src/quest/CMakeLists.txt")
        paths = self.triggers["pull_request"]["paths"]
        for source in sources:
            full = f"src/{source}"
            self.assertTrue((REPO / full).exists(), full)
            self.assertTrue(any(matches(full, p) for p in paths), f"{full} is compiled by src/quest but not in the paths")

    def test_the_job_builds_and_tests_with_the_pinned_ndk_and_vcpkg(self):
        text = WORKFLOW.read_text()
        self.assertIn("just test-android", text)
        self.assertIn("26.3.11579264", text)
        self.assertIn('.vcpkg-commit', text)
        ndk = (REPO / "src/quest/CMakePresets.json").read_text()
        self.assertIn("NDK r26d", ndk)


if __name__ == "__main__":
    unittest.main()
