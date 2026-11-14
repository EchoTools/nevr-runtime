# ADR 0044 recorded reason: tests tools/owned_remove.py, the Python removal helper that stays until its importers are ported under #2132.
"""tools/owned_remove.py: a tool removes only the entries it recorded, and a build tree only when its real path is inside
the allowed parent with no link on the way. A link planted where a tool deletes must leave its canary outside intact.
"""

from __future__ import annotations

import os
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import owned_remove  # noqa: E402
from owned_remove import Refused, remove_checked_tree, remove_owned  # noqa: E402


class RemoveOwnedTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self._tmp.name)
        self.canary = self.base / "canary"
        self.canary.mkdir()
        (self.canary / "keep.txt").write_text("canary\n")
        self.root = self.base / "root"
        self.root.mkdir()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_parent_link_is_refused_and_canary_survives(self) -> None:
        os.symlink(self.canary, self.root / "link")
        (self.root / "owned").mkdir()
        results = remove_owned(self.root, ["link/keep.txt"])
        self.assertEqual([r.outcome for r in results], ["refused_link"])
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_last_component_link_is_unlinked_and_target_survives(self) -> None:
        os.symlink(self.canary / "keep.txt", self.root / "ln")
        results = remove_owned(self.root, ["ln"])
        self.assertEqual([r.outcome for r in results], ["removed"])
        self.assertFalse(os.path.lexists(self.root / "ln"))
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_names_that_are_not_plain_are_refused(self) -> None:
        for name in ["", ".", "..", "a/../b", "/abs", "a\\b", "C:x", "a//b", "x/./y", "a\x00b", "a\x01b", "a\x7fb", "a/"]:
            with self.subTest(name=name):
                results = remove_owned(self.root, [name])
                self.assertEqual([r.outcome for r in results], ["refused_name"])
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_folder_holding_an_unlisted_file_is_kept(self) -> None:
        folder = self.root / "item"
        folder.mkdir()
        (folder / "listed.txt").write_text("a")
        (folder / "unlisted.txt").write_text("b")
        results = remove_owned(self.root, ["item/listed.txt", "item"])
        self.assertEqual([r.outcome for r in results], ["removed", "not_empty"])
        self.assertTrue((folder / "unlisted.txt").is_file())

    def test_recorded_entries_are_removed_one_by_name(self) -> None:
        (self.root / "d").mkdir()
        (self.root / "d" / "f").write_text("x")
        results = remove_owned(self.root, ["d/f", "d"])
        self.assertEqual([r.outcome for r in results], ["removed", "removed"])
        self.assertFalse(os.path.lexists(self.root / "d"))

    def test_absent_entry_is_reported_not_raised(self) -> None:
        self.assertEqual([r.outcome for r in remove_owned(self.root, ["nothing"])], ["absent"])

    def test_link_as_root_refuses_everything(self) -> None:
        linked_root = self.base / "linked-root"
        os.symlink(self.root, linked_root)
        (self.root / "f").write_text("x")
        self.assertEqual([r.outcome for r in remove_owned(linked_root, ["f"])], ["refused_link"])
        self.assertTrue((self.root / "f").is_file())


class RemoveCheckedTreeTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self._tmp.name)
        self.build = self.base / "build"
        self.build.mkdir()
        self.canary = self.base / "canary"
        self.canary.mkdir()
        (self.canary / "keep.txt").write_text("canary\n")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_tree_inside_build_is_removed(self) -> None:
        tree = self.build / "preset" / "sub"
        tree.mkdir(parents=True)
        (tree / "f").write_text("x")
        remove_checked_tree(self.build / "preset", self.build)
        self.assertFalse(os.path.lexists(self.build / "preset"))

    def test_path_outside_build_is_refused(self) -> None:
        with self.assertRaises(Refused):
            remove_checked_tree(self.canary, self.build)
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_build_itself_is_refused(self) -> None:
        with self.assertRaises(Refused):
            remove_checked_tree(self.build, self.build)
        self.assertTrue(self.build.is_dir())

    def test_link_component_is_refused_and_canary_survives(self) -> None:
        os.symlink(self.canary, self.build / "linked")
        with self.assertRaises(Refused):
            remove_checked_tree(self.build / "linked" / "keep.txt", self.build)
        self.assertEqual((self.canary / "keep.txt").read_text(), "canary\n")

    def test_link_as_the_tree_is_refused(self) -> None:
        os.symlink(self.canary, self.build / "tree")
        with self.assertRaises(Refused):
            remove_checked_tree(self.build / "tree", self.build)
        self.assertTrue((self.canary / "keep.txt").is_file())

    def test_link_as_allowed_parent_is_refused(self) -> None:
        linked_build = self.base / "linked-build"
        os.symlink(self.build, linked_build)
        (self.build / "preset").mkdir()
        with self.assertRaises(Refused):
            remove_checked_tree(linked_build / "preset", linked_build)
        self.assertTrue((self.build / "preset").is_dir())

    def test_missing_tree_is_not_an_error(self) -> None:
        remove_checked_tree(self.build / "never-made", self.build)


if __name__ == "__main__":
    unittest.main()
