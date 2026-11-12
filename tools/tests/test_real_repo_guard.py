"""tools/tests/real_repo_guard.py: a block fails on a change its tests could make to the checkout, not on another
session's worktree (#2155). The checkout is a fixture; the other session's worktree is made beside it."""
import pathlib
import tempfile
import unittest
from unittest import mock

import gitenv
import real_repo_guard


class RealRepoGuardTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name).resolve()
        self.repo = self.tmp / "checkout"
        self.repo.mkdir()
        gitenv.git(self.repo, "init", "-q", "-b", "main")
        (self.repo / "f").write_text("x\n")
        gitenv.git(self.repo, "add", "f")
        gitenv.git(self.repo, "commit", "-q", "-m", "init")
        # The temporary directory the guard counts as the tests' own: a directory of this fixture.
        self.own = self.tmp / "own"
        self.own.mkdir()
        self.elsewhere = self.tmp / "elsewhere"
        self.elsewhere.mkdir()
        for patched in (mock.patch.object(real_repo_guard, "REPO", self.repo),
                        mock.patch.object(real_repo_guard, "_own_root", create=True, return_value=self.own.resolve())):
            patched.start()
            self.addCleanup(patched.stop)

    def worktree(self, path):
        gitenv.git(self.repo, "worktree", "add", "-q", "--no-track", "-b", path.name, str(path))

    def test_a_worktree_another_session_makes_during_the_block_is_not_a_change(self):
        with real_repo_guard.real_repo_unchanged(self):
            self.worktree(self.elsewhere / "other-session")

    def test_a_worktree_the_tests_make_under_their_own_directory_is_a_change(self):
        with self.assertRaises(AssertionError):
            with real_repo_guard.real_repo_unchanged(self):
                self.worktree(self.own / "made-by-a-test")

    def test_a_lock_the_tests_take_on_their_worktree_is_a_change(self):
        made = self.own / "locked-by-a-test"
        self.worktree(made)
        with self.assertRaises(AssertionError):
            with real_repo_guard.real_repo_unchanged(self):
                gitenv.git(self.repo, "worktree", "lock", "--reason", "test", str(made))

    def test_a_submodule_config_change_is_a_change(self):
        with self.assertRaises(AssertionError):
            with real_repo_guard.real_repo_unchanged(self):
                gitenv.git(self.repo, "config", "submodule.x.url", "/somewhere")


if __name__ == "__main__":
    unittest.main()
