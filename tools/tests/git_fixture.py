"""Helpers for tests that build throwaway git repositories.

A stray GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE or GIT_COMMON_DIR in the environment makes `git -C <tmp>`
act on another repository, so every call here removes them. `real_repo_unchanged` asserts that the checkout
the tests run from keeps byte-identical submodule config and worktree list across a block.
"""
from __future__ import annotations

import contextlib
import os
import pathlib
import subprocess

REPO = pathlib.Path(__file__).resolve().parents[2]
STRIPPED_ENV = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR")


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if k not in STRIPPED_ENV}
    env.update(GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null", GIT_TERMINAL_PROMPT="0")
    env.update(extra)
    return env


def git(cwd, *args, check=True):
    """`git -C cwd args` with a clean environment, a committer and file-protocol submodules allowed."""
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "protocol.file.allow=always", "-C", str(cwd), *args],
                          check=check, capture_output=True, text=True, env=clean_env())


def real_repo_state():
    """The submodule config and the worktree paths and locks of the checkout the tests run from, as bytes.

    The worktree list is reduced to its `worktree` and `locked` lines: the HEAD each worktree reports moves
    whenever any other session commits, and a block that lasts a second must not fail on that.
    """
    env = {k: v for k, v in os.environ.items() if k not in STRIPPED_ENV}

    def run(*args):
        return subprocess.run(["git", "-C", str(REPO), *args], env=env, capture_output=True, text=True).stdout

    worktrees = [ln for ln in run("worktree", "list", "--porcelain").splitlines()
                 if ln.startswith(("worktree ", "locked"))]
    return (run("config", "--get-regexp", r"^submodule\.") + "\0" + "\n".join(worktrees)).encode()


@contextlib.contextmanager
def real_repo_unchanged(test):
    before = real_repo_state()
    yield
    test.assertEqual(real_repo_state(), before,
                     "the checkout the tests run from changed its submodule config or worktree list")
