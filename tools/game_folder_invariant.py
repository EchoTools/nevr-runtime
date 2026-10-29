"""The game-folder invariant: nothing of ours is written to the game install except BugSplat64.dll.

The game directory is the install root, the folder that holds `bin\\win10`, `_local` and the game's own data. The only
file of ours that sits in it is `bin\\win10\\BugSplat64.dll`, the host the game loads. No config.yaml, no `_local`, no
`_temp`, no logs, no install record, no set-aside or backup files, no plugins folder, no MOVED-TO notes: all of it lives
under `%LOCALAPPDATA%\\nevr\\echovr`.

`snapshot` records every entry under an install root (kind, mode, size and SHA-256 of a file, target of a link) and
`violations` lists what changed between two snapshots, other than the content of the host. `run_steps` takes a snapshot
before the first step and after each step and checks each against the first, so a step that leaves a file behind is named.

Two steps are filled in by the people who can run the game, through two environment variables, each a command line that
gets the install root as its last argument and exits 0 when the run was clean:

    NEVR_INVARIANT_CLIENT_RUN   a client run on the install (start, log in, exit)
    NEVR_INVARIANT_SERVER_RUN   a `-server` run on the install (start, register, exit)

A step whose variable is unset is reported as not run, in the result and on stderr; it is never reported as passed.
"""
from __future__ import annotations

import hashlib
import os
import pathlib
import shlex
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Callable, Mapping

HOST = "bin/win10/BugSplat64.dll"
CLIENT_RUN_ENV = "NEVR_INVARIANT_CLIENT_RUN"
SERVER_RUN_ENV = "NEVR_INVARIANT_SERVER_RUN"


def snapshot(root: pathlib.Path) -> dict[str, str]:
    """Every entry under `root` (not `root` itself), keyed by its path with `/` separators."""
    out: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        st = path.lstat()
        mode = oct(st.st_mode & 0o7777)
        if path.is_symlink():
            out[rel] = f"link {os.readlink(path)}"
        elif path.is_dir():
            out[rel] = f"dir {mode}"
        else:
            out[rel] = f"file {mode} {st.st_size} {hashlib.sha256(path.read_bytes()).hexdigest()}"
    return out


def violations(before: Mapping[str, str], after: Mapping[str, str], host: str = HOST) -> list[str]:
    """What differs between two snapshots, except the content of the host. The host itself must still be a file."""
    found: list[str] = []
    for rel in sorted(set(before) | set(after)):
        old, new = before.get(rel), after.get(rel)
        if old == new:
            continue
        if rel == host and old is not None and new is not None and old.startswith("file ") and new.startswith("file "):
            if old.split()[1] != new.split()[1]:
                found.append(f"{rel}: the mode changed ({old.split()[1]} -> {new.split()[1]})")
            continue
        if old is None:
            found.append(f"{rel}: added ({new})")
        elif new is None:
            found.append(f"{rel}: removed (was {old})")
        else:
            found.append(f"{rel}: changed ({old} -> {new})")
    return found


@dataclass
class StepResult:
    name: str
    ran: bool
    violations: list[str] = field(default_factory=list)
    error: str = ""

    @property
    def ok(self) -> bool:
        return self.ran and not self.violations and not self.error


Step = tuple[str, Callable[[pathlib.Path], None]]


def command_step(env_name: str) -> Callable[[pathlib.Path], None] | None:
    """The step an environment variable names, or None when it is unset or empty."""
    line = os.environ.get(env_name, "").strip()
    if not line:
        return None

    def run(root: pathlib.Path) -> None:
        done = subprocess.run([*shlex.split(line), str(root)], capture_output=True, text=True, timeout=1800)
        if done.returncode != 0:
            raise RuntimeError(f"{env_name} exited {done.returncode}: {(done.stdout + done.stderr)[-800:]}")

    return run


def run_steps(root: pathlib.Path, steps: list[tuple[str, Callable[[pathlib.Path], None] | None]]) -> list[StepResult]:
    """Runs each step on `root` and checks the tree against the snapshot taken before the first one.

    A step that is None is not run: it is reported so on stderr and in its result. A step that raises is a failed step and
    ends the run (later steps would act on an unknown state)."""
    initial = snapshot(root)
    results: list[StepResult] = []
    for name, step in steps:
        if step is None:
            print(f"game-folder invariant: step '{name}' NOT RUN (no command given)", file=sys.stderr)
            results.append(StepResult(name, ran=False))
            continue
        try:
            step(root)
        except Exception as e:  # a step is any callable; its failure is the result, not a crash of the check
            results.append(StepResult(name, ran=True, error=f"{type(e).__name__}: {e}"))
            break
        results.append(StepResult(name, ran=True, violations=violations(initial, snapshot(root))))
    return results
