# ADR 0044 recorded reason: tests the work root's run folders through the Python helper tools/work_root.py, which stays until #2132 ports it.
"""tools/work_root.py run roots: run_root_make and run_root_remove call build/go-tools/run-root, the one implementation. A run root that is a link, outside run-roots, or misnamed is refused with its canary intact.
"""

from __future__ import annotations

import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import work_root  # noqa: E402
from work_root import RunRootRefused, run_root_make, run_root_remove  # noqa: E402



class RunRootTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self._tmp.name)
        self.work = self.base / "work"
        self.work.mkdir()
        self.canary = self.base / "canary"
        self.canary.mkdir()
        (self.canary / "keep.txt").write_text("canary\n")
        self._env = mock.patch.dict(os.environ, {work_root.ENV_VAR: str(self.work)})
        self._env.start()

    def tearDown(self) -> None:
        self._env.stop()
        self._tmp.cleanup()

    def test_make_then_remove_removes_that_run_root_only(self) -> None:
        made = run_root_make("unit")
        self.assertRegex(made.name, r"^unit-\d{8}T\d{6}Z-\d+$")
        self.assertEqual(made.parent, self.work / "run-roots")
        (made / "scratch.txt").write_text("x")
        run_root_remove(made)
        self.assertFalse(os.path.lexists(made))
        self.assertTrue((self.work / "run-roots").is_dir())

    def test_link_run_root_is_refused_and_canary_survives(self) -> None:
        runs = self.work / "run-roots"
        runs.mkdir()
        link = runs / "unit-20261114T030405Z-1"
        os.symlink(self.canary, link)
        with self.assertRaises(RunRootRefused):
            run_root_remove(link)
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_run_root_outside_run_roots_is_refused(self) -> None:
        outside = self.work / "unit-20261114T030405Z-2"
        outside.mkdir()
        with self.assertRaises(RunRootRefused):
            run_root_remove(outside)
        self.assertTrue(outside.is_dir())

    def test_misnamed_run_root_is_refused(self) -> None:
        runs = self.work / "run-roots"
        runs.mkdir()
        bad = runs / "Not A Name"
        bad.mkdir()
        with self.assertRaises(RunRootRefused):
            run_root_remove(bad)
        self.assertTrue(bad.is_dir())

    def test_dot_dot_path_is_refused(self) -> None:
        made = run_root_make("unit")
        with self.assertRaises(RunRootRefused):
            run_root_remove(str(made) + "/..")
        self.assertTrue(made.is_dir())

    def test_run_roots_directory_that_is_a_link_is_refused(self) -> None:
        elsewhere = self.base / "elsewhere"
        elsewhere.mkdir()
        (elsewhere / "unit-20261114T030405Z-3").mkdir()
        os.symlink(elsewhere, self.work / "run-roots")
        with self.assertRaises(RunRootRefused):
            run_root_remove(self.work / "run-roots" / "unit-20261114T030405Z-3")
        self.assertTrue((elsewhere / "unit-20261114T030405Z-3").is_dir())


if __name__ == "__main__":
    unittest.main()
