# ADR 0044 recorded reason: the removal helper the Python tools that stay import; its Go twin is tools/internal/owned, and the importers end with the #2132 ports.
"""Removal that deletes only what a tool made (ORDER.md, "The manifest rule").

Two calls, and nothing else removes:

- `remove_owned(root, entries)`: each entry is a '/'-separated name relative to `root`, taken from the tool's own
  record of what it made (its manifest), never from a listing. One entry is removed by name, one call at a time. A
  folder is removed only when it is empty, so a file that is not in the manifest keeps its folder. No entry is
  recursed into. A parent component that is a link is refused, and a link that is itself the entry is unlinked (its
  target is not touched).
- `remove_checked_tree(path, allowed_parent)`: the recursive removal allowed for a build tree (a CMake binary dir, a
  vcpkg buildtree, a `build/<preset>` directory). It removes `path` only when the real path of `path` is strictly
  inside the real path of `allowed_parent`, and no component from `allowed_parent` down to `path` is a link.

The sanctioned recursive removal of a tool's own scratch is `work_root.run_root_remove` (the Python twin of the Go
`runroot.Remove`), not either of these.

Limits, stated: the checks are by path. A component replaced between the check and the removal is not caught; the
exposure needs write access to the tree the tool itself writes.
"""

from __future__ import annotations

import contextlib
import errno
import os
import pathlib
import re
import shutil
import stat
import tempfile
from dataclasses import dataclass


class Refused(RuntimeError):
    """A removal the rules do not allow. Nothing was removed by the call that raised it."""


@dataclass(frozen=True)
class Result:
    entry: str
    # removed | absent | not_empty | refused_name | refused_link | refused_outside_root | failed
    outcome: str
    detail: str = ""


CONTROL = re.compile(r"[\x00-\x1f\x7f]")


def entry_is_plain(entry: str) -> bool:
    """The grammar of src/core/owned_remove_rules.h ValidEntry (the same as installer/src/util.rs valid_entry, plus the
    control bytes that the C++ header refuses): non-empty, not absolute, no control byte (below 0x20 or 0x7F), no '\\'
    and no ':' anywhere, and every component non-empty and not '.' or '..'. A trailing '/' gives an empty component."""
    if not entry or entry.startswith("/") or CONTROL.search(entry) or "\\" in entry or ":" in entry:
        return False
    return all(part not in ("", ".", "..") for part in entry.split("/"))


_NOFOLLOW_DIR = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC


def _open_parent(dir_fd: int, part: str) -> int:
    """openat(dir_fd, part, O_DIRECTORY|O_NOFOLLOW): a link gives ELOOP or ENOTDIR (refused_link, as the POSIX guard)."""
    return os.open(part, _NOFOLLOW_DIR, dir_fd=dir_fd)


def _remove_one(root_fd: int, root_dev: int, entry: str) -> Result:
    if not entry_is_plain(entry):
        return Result(entry, "refused_name")
    parts = entry.split("/")
    opened: list[int] = []
    dir_fd = root_fd
    try:
        for part in parts[:-1]:
            try:
                fd = _open_parent(dir_fd, part)
            except FileNotFoundError:
                return Result(entry, "absent")
            except OSError as error:
                if error.errno in (errno.ELOOP, errno.ENOTDIR):
                    return Result(entry, "refused_link", part)
                return Result(entry, "failed", f"{part}: {error.strerror}")
            opened.append(fd)
            if os.fstat(fd).st_dev != root_dev:
                return Result(entry, "refused_outside_root", part)
            dir_fd = fd
        last = parts[-1]
        try:
            mode = os.stat(last, dir_fd=dir_fd, follow_symlinks=False).st_mode
        except FileNotFoundError:
            return Result(entry, "absent")
        if stat.S_ISDIR(mode):
            os.rmdir(last, dir_fd=dir_fd)
        else:
            os.unlink(last, dir_fd=dir_fd)
        return Result(entry, "removed")
    except OSError as error:
        if error.errno in (errno.ENOTEMPTY, errno.EEXIST):
            return Result(entry, "not_empty", entry)
        return Result(entry, "failed", f"{entry}: {error.strerror}")
    finally:
        for fd in opened:
            os.close(fd)


def remove_owned(root, entries) -> list[Result]:
    """Remove each recorded entry under `root`, in the order given, one Result each. The root is opened once with
    O_NOFOLLOW, and every parent is opened relative to the one before it, so no path is looked up again after its check
    (the POSIX guard, src/core/owned_remove_posix.cpp). Needs os.open with dir_fd (POSIX)."""
    root_text = os.fspath(root)
    if CONTROL.search(root_text):
        return [Result(entry, "refused_name", "root has a control byte") for entry in entries]
    try:
        root_fd = os.open(root_text, _NOFOLLOW_DIR)
    except OSError as error:
        outcome = "refused_link" if error.errno in (errno.ELOOP, errno.ENOTDIR) else "failed"
        return [Result(entry, outcome, f"{root_text}: {error.strerror}") for entry in entries]
    try:
        root_dev = os.fstat(root_fd).st_dev
        return [_remove_one(root_fd, root_dev, entry) for entry in entries]
    finally:
        os.close(root_fd)


def remove_checked_tree(path, allowed_parent) -> None:
    """Remove the build tree `path` recursively, if and only if it is strictly inside `allowed_parent` by real path and
    no link lies between them. Raises Refused otherwise, and removes nothing. A missing `path` is not an error."""
    parent = pathlib.Path(os.path.abspath(allowed_parent))
    target = pathlib.Path(os.path.abspath(path))
    if os.path.islink(parent):
        raise Refused(f"refused: {parent} is a link")
    relative = os.path.relpath(target, parent)
    if relative in (".", "") or relative == os.pardir or relative.startswith(os.pardir + os.sep):
        raise Refused(f"refused: {target} is not inside {parent}")
    current = parent
    for part in pathlib.PurePath(relative).parts:
        current = current / part
        if not os.path.lexists(current):
            return
        if os.path.islink(current):
            raise Refused(f"refused: {current} is a link")
    real_parent = os.path.realpath(parent)
    real_target = os.path.realpath(target)
    if not real_target.startswith(real_parent + os.sep) or real_target == real_parent:
        raise Refused(f"refused: real path {real_target} is not strictly inside {real_parent}")
    if os.path.isdir(target) and not os.path.islink(target):
        shutil.rmtree(target)
    else:
        os.unlink(target)


@contextlib.contextmanager
def owned_scratch(parent, prefix: str):
    """A scratch directory the tool makes under `parent`, on the same filesystem as the outputs it renames into place
    (a TemporaryDirectory cannot be used there without its unchecked removal). It is removed on exit by
    remove_checked_tree, with `parent` as the allowed parent, so a link on the way refuses the removal."""
    parent = pathlib.Path(parent)
    parent.mkdir(parents=True, exist_ok=True)
    path = pathlib.Path(tempfile.mkdtemp(prefix=prefix, dir=parent))
    try:
        yield path
    finally:
        remove_checked_tree(path, parent)
