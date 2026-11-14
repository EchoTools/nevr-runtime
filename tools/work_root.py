"""The work root: where every tool of this repository keeps what it writes outside the checkout.

The root is $NEVR_WORK_ROOT, else `src/nevr-runtime-work` under the user's home directory. Every
Python tool takes its default from here and spells no path of its own; the shell side is
tools/lib/work_root.sh and nothing ties the two to the same answer yet (no test compares them).

A directory a tool creates under the root carries a README.md saying what it is for, whose it is and
when it is removed (AGENTS.md, "Working practices for every agent"). `ensure_dir` makes the
directory and writes that README at the top-level directory under the root and at the root itself;
it never overwrites a README that exists.
"""

from __future__ import annotations

import contextlib
import getpass
import os
import pathlib
import re
import subprocess

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


class RunRootRefused(RuntimeError):
    """A run root the Go tool refused (make or remove). Nothing was removed."""


REPO = pathlib.Path(__file__).resolve().parents[1]
RUN_ROOT_BINARY = REPO / "build" / "go-tools" / "run-root"


def run_root_binary() -> pathlib.Path:
    """build/go-tools/run-root, the one implementation of run-root make and remove (tools/internal/runroot). It is built
    from tools/cmd/run-root when missing, with the same command `just go-tools` runs for it."""
    if not RUN_ROOT_BINARY.exists():
        out = REPO / "build" / "go-tools"
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run(["go", "-C", str(REPO / "tools"), "build", "-trimpath", "-o", str(out) + os.sep, "./cmd/run-root"],
                       check=True)
    return RUN_ROOT_BINARY


def run_root_make(label: str) -> pathlib.Path:
    """Make a run root under the work root with the Go tool; returns its path."""
    done = subprocess.run([str(run_root_binary()), "make", label], capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise RunRootRefused(f"run-root make {label}: {done.stderr.strip() or done.returncode}")
    return pathlib.Path(done.stdout.strip())


def run_root_remove(path) -> None:
    """Remove one run root with the Go tool, which makes the checks of runroot.Remove. Refused means nothing removed."""
    done = subprocess.run([str(run_root_binary()), "remove", str(path)], capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise RunRootRefused(f"run-root remove {path}: {done.stderr.strip() or done.returncode}")


@contextlib.contextmanager
def run_root(label: str):
    """A run root for the length of a with-block: made by the Go tool, removed once by it on exit."""
    path = run_root_make(label)
    try:
        yield path
    finally:
        run_root_remove(path)


class RunRootDir:
    """What make_test_dir returns: `name` is a run root made by the Go tool. cleanup() (or leaving a with-block) removes it
    with the Go tool's remove; nothing else deletes it."""

    def __init__(self, label: str) -> None:
        self.path: pathlib.Path | None = run_root_make(label)
        self.name = str(self.path)

    def cleanup(self) -> None:
        if self.path is not None:
            run_root_remove(self.path)
            self.path = None

    def __enter__(self) -> str:
        return self.name

    def __exit__(self, *exc: object) -> None:
        self.cleanup()


def make_test_dir(prefix: str = "tmp-") -> RunRootDir:
    """A run root (made by the Go tool) for a test's scratch. The label is the prefix reduced to the characters a run root
    label allows; the directory is removed by cleanup() or on leaving a with-block."""
    label = re.sub(r"[^a-z0-9-]+", "-", prefix.lower()).strip("-") or "tmp"
    return RunRootDir(label[:64])
