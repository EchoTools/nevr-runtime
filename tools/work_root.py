"""The work root: where every tool of this repository keeps what it writes outside the checkout.

The root is $NEVR_WORK_ROOT, else `src/nevr-runtime-work` under the user's home directory. Every
Python tool takes its default from here and spells no path of its own; the shell side is
tools/lib/work_root.sh and tools/tests/test_work_dir_contract.py holds the two to the same answer.

A directory a tool creates under the root carries a README.md saying what it is for, whose it is and
when it is removed (AGENTS.md, "Working practices for every agent"). `ensure_dir` makes the
directory and writes that README at the top-level directory under the root and at the root itself;
it never overwrites a README that exists.
"""

from __future__ import annotations

import getpass
import os
import pathlib
import tempfile

ENV_VAR = "NEVR_WORK_ROOT"
DEFAULT_RELATIVE = pathlib.Path("src") / "nevr-runtime-work"

ROOT_PURPOSE = "Working files of the nevr-runtime tools and the agents that run them."
ROOT_REMOVED = "each item when its named end comes; its creator removes it that turn."


def work_root() -> pathlib.Path:
    """$NEVR_WORK_ROOT, else ~/src/nevr-runtime-work. Read on every call, so a test can set the variable."""
    override = os.environ.get(ENV_VAR)
    if override:
        return pathlib.Path(override)
    return pathlib.Path.home() / DEFAULT_RELATIVE


def _owner() -> str:
    try:
        return getpass.getuser()
    except (KeyError, OSError):
        return "the user who ran the tool"


def _write_readme(directory: pathlib.Path, purpose: str, removed: str) -> None:
    readme = directory / "README.md"
    text = f"# {directory.name}\n\n{purpose}\n\nOwner: {_owner()}\nRemoved: {removed}\n"
    try:
        with open(readme, "x", encoding="utf-8") as handle:
            handle.write(text)
    except FileExistsError:
        pass


def ensure_dir(path: pathlib.Path, purpose: str, removed: str) -> pathlib.Path:
    """Create `path` (and its parents). When it is under the work root, write the README of the
    top-level directory it sits in, from `purpose` and `removed`, and the root's own if it is missing.
    A path outside the root (a per-tool override) is only created. Raises OSError, loud, when the
    directory or a README cannot be written."""
    path = pathlib.Path(path)
    path.mkdir(parents=True, exist_ok=True)
    root = work_root()
    try:
        relative = path.resolve().relative_to(root.resolve())
    except ValueError:
        return path
    root.mkdir(parents=True, exist_ok=True)
    _write_readme(root, ROOT_PURPOSE, ROOT_REMOVED)
    if relative.parts:
        _write_readme(root / relative.parts[0], purpose, removed)
    return path


def test_scratch() -> pathlib.Path:
    """The directory the tools' own tests make their temporary directories in (each test removes its
    own when the test ends)."""
    return ensure_dir(
        work_root() / "test-scratch",
        "Temporary directories of the tests under tools/tests; each test removes its own when it ends.",
        "a directory in here left behind by an interrupted test run can be removed at any time; this directory when no test run is live.",
    )


def make_test_dir(prefix: str = "tmp-") -> tempfile.TemporaryDirectory:
    """A TemporaryDirectory in the test scratch directory."""
    return tempfile.TemporaryDirectory(prefix=prefix, dir=test_scratch())
