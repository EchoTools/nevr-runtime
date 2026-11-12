"""A guard for tests that build throwaway repositories next to the checkout they run from.

The git calls themselves go through tools/tests/gitenv.py (it drops GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE and
GIT_COMMON_DIR). `real_repo_unchanged` adds one more assertion for the tests that add submodules and worktrees: the
checkout the tests run from keeps its submodule config and the paths and locks of its worktrees across a block.
"""
from __future__ import annotations

import contextlib
import pathlib
import subprocess
import tempfile

import gitenv

REPO = pathlib.Path(__file__).resolve().parents[2]
# Environment for gitenv.git: submodules cloned from a local path, and no user or system git config.
SUBMODULE_ENV = {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "protocol.file.allow", "GIT_CONFIG_VALUE_0": "always",
                 "GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_SYSTEM": "/dev/null", "GIT_TERMINAL_PROMPT": "0"}


def _own_root() -> pathlib.Path:
    """The directory the tests make their throwaway worktrees in: the temporary directory (#2155)."""
    return pathlib.Path(tempfile.gettempdir()).resolve()


def _worktrees_the_tests_could_change(listing: str) -> list:
    """The `worktree` and `locked` lines of the entries under the temporary directory, in listing order.

    Another session's worktrees live in its own work root, never in the temporary directory, so a block cannot have
    changed them. The HEAD an entry reports is left out: it moves whenever any session commits.
    """
    kept = []
    for block in listing.split("\n\n"):
        lines = [ln for ln in block.splitlines() if ln]
        if not lines or not lines[0].startswith("worktree "):
            continue
        path = pathlib.Path(lines[0][len("worktree "):])
        if path.is_relative_to(_own_root()):
            kept += [ln for ln in lines if ln.startswith(("worktree ", "locked"))]
    return kept


def real_repo_state():
    """The submodule config and the worktrees the tests could have made, and their locks, of the checkout as bytes.

    A block that adds, removes or locks a worktree of the checkout under the temporary directory changes this; a
    worktree another session makes or removes does not (#2155).
    """
    def run(*args):
        return gitenv.git(REPO, *args, check=False).stdout

    worktrees = _worktrees_the_tests_could_change(run("worktree", "list", "--porcelain"))
    return (run("config", "--get-regexp", r"^submodule\.") + "\0" + "\n".join(worktrees)).encode()


@contextlib.contextmanager
def real_repo_unchanged(test):
    before = real_repo_state()
    yield
    test.assertEqual(real_repo_state(), before,
                     "the checkout the tests run from changed its submodule config or worktree list")
