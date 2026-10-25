#!/usr/bin/env python3
"""Check that a PR title is a Conventional Commits 1.0.0 header: `type(scope)!: description`.

Spec: https://www.conventionalcommits.org/en/v1.0.0/. The scope and the `!` are optional; the type is one of
TYPES, matched case-insensitively (spec item 15); the description is non-empty. Nothing is required of the
description's case. Standard library only.

    PR_TITLE=... check_pr_title.py
    check_pr_title.py --title "fix(boot): accept --server"

Exit 0 and one `ok:` line on success; exit 1 and one `FAIL:` line on stderr otherwise.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

TYPES = ("feat", "fix", "docs", "style", "refactor", "perf", "test", "build", "ci", "chore", "revert")

HEADER = re.compile(r"^(?P<type>[A-Za-z]+)(?:\((?P<scope>[^()\s][^()]*)\))?(?P<bang>!)?: (?P<description>\S.*)$")


class Failure(Exception):
    pass


def check(title: str) -> str:
    """Return the lowercased type when the title is a conventional header; raise Failure with one line otherwise."""
    if "\n" in title or "\r" in title:
        raise Failure("the PR title must be one line")
    match = HEADER.match(title)
    if match is None:
        raise Failure(f"{title!r} is not `type(scope)!: description` (https://www.conventionalcommits.org/en/v1.0.0/); "
                      "the scope and the ! are optional, the colon and the space after it are not")
    kind = match.group("type").lower()
    if kind not in TYPES:
        raise Failure(f"unknown type {match.group('type')!r}; use one of: {', '.join(TYPES)}")
    return kind


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--title", help="the PR title (default: $PR_TITLE)")
    args = parser.parse_args(argv)
    title = args.title if args.title is not None else os.environ.get("PR_TITLE")
    if title is None:
        print("FAIL: no title given (--title or PR_TITLE)", file=sys.stderr)
        return 1
    try:
        kind = check(title)
    except Failure as failure:
        print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"ok: {title!r} is a conventional `{kind}` title")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
