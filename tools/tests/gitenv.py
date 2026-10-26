"""The git helper for tests that build a throwaway repository (test_gitenv.py proves it).

`git -C <dir>` names the directory to run in, but GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE and GIT_COMMON_DIR in the
environment override it. A test run that inherits them (from a git hook, an editor or a wrapper) would send
`git init`, `git commit`, `git submodule add` and `git worktree add` to the repository they name, which can be the
real one. Every test that runs git on a repository it made imports `git` from here instead of calling subprocess
itself."""
from __future__ import annotations

import os
import subprocess

LEAKING_VARIABLES = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR")


def clean_env(base=None):
    """A copy of `base` (default: the environment) without the variables that override `git -C`."""
    return {k: v for k, v in (os.environ if base is None else base).items() if k not in LEAKING_VARIABLES}


def git(cwd, *args, check=True, env=None):
    """Run git in `cwd` as a throwaway identity, capturing text output; raises CalledProcessError unless check=False.
    `env` adds variables (GIT_CONFIG_COUNT and the like) to the environment; the four above are dropped from the result."""
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-C", str(cwd), *args],
                          check=check, capture_output=True, text=True, env=clean_env({**os.environ, **(env or {})}))
