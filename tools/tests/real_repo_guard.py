"""A guard for tests that build throwaway repositories next to the checkout they run from.

The git calls themselves go through tools/tests/gitenv.py (it drops GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE and
GIT_COMMON_DIR). `real_repo_unchanged` adds one more assertion for the tests that add submodules and worktrees: the
checkout the tests run from keeps its submodule config and the paths and locks of its worktrees across a block.
"""
from __future__ import annotations

import contextlib
import pathlib
import subprocess

import gitenv

REPO = pathlib.Path(__file__).resolve().parents[2]
# Environment for gitenv.git: submodules cloned from a local path, and no user or system git config.
SUBMODULE_ENV = {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "protocol.file.allow", "GIT_CONFIG_VALUE_0": "always",
                 "GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_SYSTEM": "/dev/null", "GIT_TERMINAL_PROMPT": "0"}


def real_repo_state():
    """The submodule config and the worktree paths and locks of the checkout the tests run from, as bytes.

    The worktree list is reduced to its `worktree` and `locked` lines: the HEAD each worktree reports moves
    whenever any other session commits, and a block that lasts a second must not fail on that.
    """
    def run(*args):
        return gitenv.git(REPO, *args, check=False).stdout

    worktrees = [ln for ln in run("worktree", "list", "--porcelain").splitlines()
                 if ln.startswith(("worktree ", "locked"))]
    return (run("config", "--get-regexp", r"^submodule\.") + "\0" + "\n".join(worktrees)).encode()


@contextlib.contextmanager
def real_repo_unchanged(test):
    before = real_repo_state()
    yield
    test.assertEqual(real_repo_state(), before,
                     "the checkout the tests run from changed its submodule config or worktree list")
