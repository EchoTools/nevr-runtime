"""tools/tests/gitenv.py: the git helper every test that builds a throwaway repository uses.

A test calls `git -C <tmp> ...` on a repository it made. GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE and GIT_COMMON_DIR in the
environment override `-C`, so a run that inherits them (a git hook, an editor, a wrapper script) would send `git init`,
`git commit`, `git submodule add` and `git worktree add` to the repository those variables name: the real one. The helper
drops them from the environment it passes, and these tests prove it with a sentinel repository that must come out
byte-identical."""
from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gitenv  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
SUITES = ("tools/tests/test_seed_build_tree.py", "tools/tests/test_worktree_setup.py")


def raw_git(cwd, *args, env=None):
    """git exactly as a careless test would run it: the inherited environment, nothing removed."""
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-C", str(cwd), *args],
                          capture_output=True, text=True, env=env)


def digest(git_dir):
    """Every file under a .git directory, its path and its bytes."""
    h = hashlib.sha256()
    for path in sorted(pathlib.Path(git_dir).rglob("*")):
        h.update(str(path.relative_to(git_dir)).encode() + b"\0")
        if path.is_file() and not path.is_symlink():
            h.update(path.read_bytes())
    return h.hexdigest()


def state(repo):
    """What the incident changed in the real repository: its submodule config and its worktree list."""
    config = subprocess.run(["git", "-C", str(repo), "config", "--get-regexp", r"^submodule\."], capture_output=True)
    listing = subprocess.run(["git", "-C", str(repo), "worktree", "list", "--porcelain"], capture_output=True)
    return config.stdout, listing.stdout


class Sentinel(unittest.TestCase):
    """A repository standing in for the real one, named by the four variables."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="gitenv-test-"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.sentinel = self.tmp / "sentinel"
        self.sentinel.mkdir()
        gitenv.git(self.sentinel, "init", "-q", "-b", "main")
        gitenv.git(self.sentinel, "commit", "-q", "--allow-empty", "--no-gpg-sign", "-m", "sentinel")
        gitenv.git(self.sentinel, "config", "submodule.extern/luau.url", "https://example.invalid/luau.git")
        gitenv.git(self.sentinel, "worktree", "add", "-q", "--no-track", "-b", "linked", str(self.tmp / "sentinel-linked"))
        self.leak = {
            "GIT_DIR": str(self.sentinel / ".git"), "GIT_WORK_TREE": str(self.sentinel),
            "GIT_INDEX_FILE": str(self.sentinel / ".git/index"), "GIT_COMMON_DIR": str(self.sentinel / ".git"),
        }
        self.other = self.tmp / "other"
        self.other.mkdir()
        self.before = digest(self.sentinel / ".git")
        self.state_before = state(self.sentinel)


class Helper(Sentinel):
    def test_the_variables_the_helper_drops_are_the_four_that_override_dash_C(self):
        self.assertEqual(set(gitenv.LEAKING_VARIABLES), {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR"})

    def test_without_the_helper_the_variables_do_redirect_git(self):
        """The control: the sentinel is real, a careless call lands in it (this is why the helper exists)."""
        env = dict(os.environ, **self.leak)
        top = raw_git(self.other, "rev-parse", "--show-toplevel", env=env)
        self.assertEqual(pathlib.Path(top.stdout.strip()).resolve(), self.sentinel.resolve(), top)

    def test_the_helper_runs_in_the_directory_it_is_given_whatever_the_environment_says(self):
        with mock.patch.dict(os.environ, self.leak):
            gitenv.git(self.other, "init", "-q", "-b", "main")
            top = gitenv.git(self.other, "rev-parse", "--show-toplevel").stdout.strip()
        self.assertEqual(pathlib.Path(top).resolve(), self.other.resolve())
        self.assertTrue((self.other / ".git").is_dir())
        self.assertEqual(digest(self.sentinel / ".git"), self.before)

    def test_a_mutating_command_cannot_reach_the_repository_the_variables_name(self):
        with mock.patch.dict(os.environ, self.leak):
            gitenv.git(self.other, "init", "-q", "-b", "main")
            gitenv.git(self.other, "commit", "-q", "--allow-empty", "--no-gpg-sign", "-m", "x")
            gitenv.git(self.other, "config", "submodule.extern/luau.url", "https://example.invalid/x.git")
            gitenv.git(self.other, "worktree", "add", "-q", "--no-track", "-b", "wt", str(self.tmp / "other-wt"))
        self.assertEqual(digest(self.sentinel / ".git"), self.before)
        self.assertEqual(state(self.sentinel), self.state_before)
        self.assertNotIn(b"other-wt", state(self.sentinel)[1])

    def test_clean_env_keeps_everything_else(self):
        base = {"PATH": "/bin", "HOME": "/h", **self.leak}
        self.assertEqual(gitenv.clean_env(base), {"PATH": "/bin", "HOME": "/h"})
        self.assertIn("GIT_DIR", base, "the caller's mapping is not modified")

    def test_a_failing_command_raises_unless_told_not_to(self):
        with self.assertRaises(subprocess.CalledProcessError):
            gitenv.git(self.other, "rev-parse", "HEAD")
        self.assertNotEqual(gitenv.git(self.other, "rev-parse", "HEAD", check=False).returncode, 0)


class Suites(Sentinel):
    def test_running_the_two_suites_with_the_variables_set_leaves_the_repository_they_name_unchanged(self):
        env = dict(os.environ, **self.leak)
        env.pop("PYTEST_ADDOPTS", None)
        proc = subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider", *SUITES],
                              cwd=REPO, env=env, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout[-3000:] + proc.stderr[-1000:])
        self.assertEqual(digest(self.sentinel / ".git"), self.before)
        self.assertEqual(state(self.sentinel), self.state_before)


if __name__ == "__main__":
    unittest.main()
