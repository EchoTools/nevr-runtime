# ADR 0044 recorded reason: tests the CMake build-tree removal, which stays Python until its importers are ported under #2132.
"""cmake/nevr_build_tree_remove.cmake: the checked build tree removal, run as `cmake -P` with a planted link. A tree inside
the allowed parent is removed; a link on the way, or a path outside the allowed parent, stops the step and leaves the
canary intact.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
MODULE = REPO / "cmake" / "nevr_build_tree_remove.cmake"
CMAKE = shutil.which("cmake")


def run_step(path: pathlib.Path, allowed: pathlib.Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [CMAKE, f"-DNEVR_BT_PATH={path}", f"-DNEVR_BT_ALLOWED={allowed}", "-P", str(MODULE)],
        capture_output=True, text=True, check=False)


@unittest.skipIf(CMAKE is None, "cmake is not on PATH")
class BuildTreeRemoveCmakeTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self._tmp.name)
        self.build = self.base / "build"
        (self.build / "preset" / "sub").mkdir(parents=True)
        (self.build / "preset" / "sub" / "f").write_text("x")
        self.canary = self.base / "canary"
        self.canary.mkdir()
        (self.canary / "keep.txt").write_text("canary\n")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_tree_inside_allowed_parent_is_removed(self) -> None:
        result = run_step(self.build / "preset", self.build)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(os.path.lexists(self.build / "preset"))

    def test_missing_tree_is_not_an_error(self) -> None:
        self.assertEqual(run_step(self.build / "never", self.build).returncode, 0)

    def test_path_outside_is_refused(self) -> None:
        result = run_step(self.canary, self.build)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_link_component_is_refused_and_canary_survives(self) -> None:
        os.symlink(self.canary, self.build / "linked")
        result = run_step(self.build / "linked", self.build)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symbolic link", result.stderr + result.stdout)
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_link_as_the_tree_is_refused_and_target_survives(self) -> None:
        os.symlink(self.canary, self.build / "tree")
        result = run_step(self.build / "tree", self.build)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.canary / "keep.txt").is_file())

    def test_allowed_parent_itself_is_refused(self) -> None:
        self.assertNotEqual(run_step(self.build, self.build).returncode, 0)
        self.assertTrue((self.build / "preset").is_dir())


if __name__ == "__main__":
    unittest.main()
