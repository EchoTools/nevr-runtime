#!/usr/bin/env python3
"""Print the next release's tag, computed from the Conventional Commits since the last release tag.

The rule (docs/standards/planning.md, "Release versions"): the base is the newest `vX.Y.Z` tag reachable from REV
(a pre-release tag such as `v5.7.0-rc.1` is not a release tag; a scratch dry-run tag `v0.0.<issue>` sorts below every
release, so it is never the newest). Over the
commits after it, a breaking change (a `!` before the colon, or a body line starting `BREAKING CHANGE:` or
`BREAKING-CHANGE:`) bumps the major; otherwise a `feat` bumps the minor; otherwise a `fix` or `perf` bumps the patch.
Only a commit's first line gives its type, matched case-insensitively (Conventional Commits 1.0.0, item 15). Any
other type bumps nothing. Standard library only.

    next_release_version.py [--repo DIR] [--rev REV]

Exit 0 and the tag (`v5.6.0`) on stdout; one `next_release_version:` line per counted commit on stderr.
Exit 1 when no commit since the base bumps anything ("nothing to release"). Exit 2 when git cannot be read or there
is no release tag.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys

RELEASE_TAG = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
HEADER = re.compile(r"^(?P<type>[A-Za-z]+)(?:\([^()]*\))?(?P<bang>!)?: \S")
FOOTER = re.compile(r"^BREAKING[ -]CHANGE: ")
MAJOR, MINOR, PATCH, NONE = 3, 2, 1, 0
NAMES = {MAJOR: "major", MINOR: "minor", PATCH: "patch", NONE: "none"}


class Failure(Exception):
    pass


def git(repo: str, *args: str) -> str:
    result = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise Failure(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def base_tag(repo: str, rev: str) -> tuple[str, tuple[int, int, int]]:
    best = None
    for name in git(repo, "tag", "--merged", rev, "--list", "v*").split():
        match = RELEASE_TAG.match(name)
        if match is None:
            continue
        version = tuple(int(part) for part in match.groups())
        if best is None or version > best[1]:
            best = (name, version)
    if best is None:
        raise Failure(f"no release tag (vX.Y.Z) is reachable from {rev}")
    return best


def bump_of(message: str) -> int:
    lines = message.splitlines()
    match = HEADER.match(lines[0]) if lines else None
    if match is None:
        return NONE
    if match.group("bang") or any(FOOTER.match(line) for line in lines[1:]):
        return MAJOR
    kind = match.group("type").lower()
    if kind == "feat":
        return MINOR
    if kind in ("fix", "perf"):
        return PATCH
    return NONE


def next_version(version: tuple[int, int, int], bump: int) -> tuple[int, int, int]:
    major, minor, patch = version
    if bump == MAJOR:
        return (major + 1, 0, 0)
    if bump == MINOR:
        return (major, minor + 1, 0)
    return (major, minor, patch + 1)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=".", help="the repository (default: the current directory)")
    parser.add_argument("--rev", default="HEAD", help="the commit the release is cut from (default: HEAD)")
    args = parser.parse_args(argv)
    try:
        tag, version = base_tag(args.repo, args.rev)
        log = git(args.repo, "log", "--format=%H%x1f%B%x1e", f"{tag}..{args.rev}")
    except Failure as failure:
        print(f"next_release_version: FAIL {failure}", file=sys.stderr)
        return 2
    print(f"next_release_version: base={tag} rev={args.rev}", file=sys.stderr)
    highest = NONE
    for record in log.split("\x1e"):
        record = record.strip("\n")
        if not record:
            continue
        sha, _, message = record.partition("\x1f")
        bump = bump_of(message)
        highest = max(highest, bump)
        subject = message.splitlines()[0] if message else ""
        print(f"next_release_version: commit={sha[:12]} bump={NAMES[bump]} subject={subject}", file=sys.stderr)
    if highest == NONE:
        print(f"next_release_version: nothing to release since {tag} (no feat, fix, perf or breaking change)",
              file=sys.stderr)
        return 1
    result = "v%d.%d.%d" % next_version(version, highest)
    print(f"next_release_version: bump={NAMES[highest]} next={result}", file=sys.stderr)
    print(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
