# ADR 0044 recorded reason: tests the shell removal wrapper tools/lib/owned.sh, which stays Python-tested until #2132 ports the scripts it serves.
"""tools/lib/owned.sh: owned_build_tree_rm removes a path only when its real path is strictly inside the allowed parent and no
component on the way is a link. A link planted on the way must leave its canary outside intact.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
LIB = REPO / "tools" / "lib" / "owned.sh"


def call(path: pathlib.Path, parent: pathlib.Path) -> subprocess.CompletedProcess:
    script = 'source "$1"; owned_build_tree_rm "$2" "$3"'
    return subprocess.run(["bash", "-c", script, "_", str(LIB), str(path), str(parent)],
                          capture_output=True, text=True, check=False)


class OwnedBuildTreeShTests(unittest.TestCase):
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

    def test_tree_inside_parent_is_removed(self) -> None:
        result = call(self.build / "preset", self.build)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(os.path.lexists(self.build / "preset"))

    def test_absent_path_is_not_an_error(self) -> None:
        self.assertEqual(call(self.build / "never", self.build).returncode, 0)

    def test_path_outside_parent_is_refused(self) -> None:
        result = call(self.canary, self.build)
        self.assertEqual(result.returncode, 2)
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_dot_dot_escape_is_refused(self) -> None:
        result = call(self.build / ".." / "canary", self.build)
        self.assertEqual(result.returncode, 2)
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_parent_itself_is_refused(self) -> None:
        self.assertEqual(call(self.build, self.build).returncode, 2)
        self.assertTrue((self.build / "preset").is_dir())

    def test_link_component_is_refused_and_canary_survives(self) -> None:
        os.symlink(self.canary, self.build / "linked")
        result = call(self.build / "linked" / "keep.txt", self.build)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_link_as_the_path_is_refused_and_target_survives(self) -> None:
        os.symlink(self.canary, self.build / "tree")
        result = call(self.build / "tree", self.build)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertTrue((self.canary / "keep.txt").is_file())

    def test_link_as_the_parent_is_refused(self) -> None:
        linked = self.base / "linked-build"
        os.symlink(self.build, linked)
        result = call(linked / "preset", linked)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertTrue((self.build / "preset").is_dir())


if __name__ == "__main__":
    unittest.main()
