#!/usr/bin/env python3
"""The version a build stamps, in Python: the mirror of cmake/nevr_build_stamp.cmake (#1935).

The version is the committed VERSION file at the repo root (exactly X.Y.Z). The commit and the distance from the
nearest release tag come from git when the stamp is read. A release is a CI build exactly on the tag vX.Y.Z of
VERSION (GITHUB_ACTIONS=true, GITHUB_REF_TYPE=tag, GITHUB_REF_NAME=vX.Y.Z, distance 0); it stamps X.Y.Z and refuses a
describe string that ends in -dirty. Every other build stamps X.Y.Z-dev.<distance>+<short commit>.

The CMake script is the one the build uses; this mirror is what the packaging tools (tools/quest_apk_version.py,
tools/release_preflight.py) ask. tools/tests/test_build_stamp.py runs both on the same repositories and fails when
they differ.

  build_version.py [<repo>]     print the stamp of the repo (default: this checkout) as one JSON line

Every git command is read-only: rev-parse, describe and status are the only ones here.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
VERSION_FILE = "VERSION"
XYZ = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
NEAREST = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-([0-9]+)-g([0-9a-f]+)")
FULL_COMMIT = re.compile(r"[0-9a-f]{40}")


class BuildVersionError(Exception):
    """The version cannot be stamped: a refusal that names what is wrong (the CMake script fails the same way)."""


@dataclass(frozen=True)
class Stamp:
    version: str            # X.Y.Z from VERSION
    project_version: str    # X.Y.Z for a release, X.Y.Z-dev.<distance>+<short commit> otherwise
    is_release: bool
    distance: int           # commits since the nearest reachable release tag
    commit_full: str
    commit_short: str
    describe: str           # git describe --always --dirty --broken --abbrev=12
    is_dirty: bool


def _git(repo: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=repo, capture_output=True, text=True)


def stamp(repo: Path = REPO, environ=None) -> Stamp:
    """The stamp of the checkout at `repo`, read now. Raises BuildVersionError on a refusal."""
    environ = os.environ if environ is None else environ
    repo = Path(repo)
    version_path = repo / VERSION_FILE
    try:
        raw = version_path.read_text(encoding="utf-8")
    except OSError as error:
        raise BuildVersionError(f"{version_path}: cannot read the committed version ({error.strerror})") from None
    text = raw[:-1] if raw.endswith("\n") else raw
    if XYZ.fullmatch(text) is None:
        raise BuildVersionError(f"{version_path} must hold exactly X.Y.Z (for example 6.15.1), found {text!r}")

    near = _git(repo, "describe", "--tags", "--long", "--abbrev=4", "--match", "v[0-9]*", "--exclude", "v*-*")
    if near.returncode != 0:
        raise BuildVersionError("no v<X>.<Y>.<Z> tag is reachable from HEAD (git describe: "
                                f"{near.stderr.strip()}). Fetch the tags (git fetch --tags; in CI use fetch-depth: 0).")
    match = NEAREST.fullmatch(near.stdout.strip())
    if match is None:
        raise BuildVersionError(f"cannot parse `git describe` output {near.stdout.strip()!r} "
                                "(expected v<X>.<Y>.<Z>, then -<distance>-g<sha>)")
    tag = f"v{match.group(1)}.{match.group(2)}.{match.group(3)}"
    distance = int(match.group(4))

    describe = _git(repo, "describe", "--always", "--dirty", "--broken", "--abbrev=12")
    full = _git(repo, "rev-parse", "HEAD")
    short = _git(repo, "rev-parse", "--short", "HEAD")
    if describe.returncode != 0 or not describe.stdout.strip():
        raise BuildVersionError(f"git describe --always --dirty --broken failed in {repo}")
    if full.returncode != 0 or FULL_COMMIT.fullmatch(full.stdout.strip()) is None:
        raise BuildVersionError(f"git rev-parse HEAD did not print a 40-hex commit in {repo}")
    if short.returncode != 0 or not re.fullmatch(r"[0-9a-f]+", short.stdout.strip()):
        raise BuildVersionError(f"git rev-parse --short HEAD failed in {repo}")
    describe_text = describe.stdout.strip()
    dirty = describe_text.endswith("-dirty")

    release = (environ.get("GITHUB_ACTIONS") == "true" and environ.get("GITHUB_REF_TYPE") == "tag"
               and environ.get("GITHUB_REF_NAME") == f"v{text}" and tag == f"v{text}" and distance == 0)
    if release:
        if dirty:
            raise BuildVersionError(f"a release build of v{text} refuses to build: its describe string is "
                                    f"{describe_text!r}, which ends in -dirty")
        project = text
    else:
        project = f"{text}-dev.{distance}+{short.stdout.strip()}"
    return Stamp(version=text, project_version=project, is_release=release, distance=distance,
                 commit_full=full.stdout.strip(), commit_short=short.stdout.strip(), describe=describe_text,
                 is_dirty=dirty)


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    repo = Path(argv[0]) if argv else REPO
    try:
        result = stamp(repo)
    except BuildVersionError as error:
        print(json.dumps({"event": "build_version_refused", "reason": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps({"version": result.version, "project_version": result.project_version,
                      "is_release": result.is_release, "describe": result.describe}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
