#!/usr/bin/env python3
"""The per-audience section of a release body, from the feat, fix and perf commits between two refs (RELEASING.md,
"Write the release body"). Usage: release_audience_section.py --repo <dir> --base vX.Y.Z --rev vX.Y.Z

A commit's audience is its conventional scope: `plugins` is for plugin developers, `server` and `ws-bridge` for server
hosts, and every other scope, or none, is for players. A breaking change (`!`) is listed under its audience with
"BREAKING:" in front. The section ends with what was not tested in the build, which is the same for every release.
"""

import argparse
import re
import subprocess
import sys

AUDIENCES = ("For players", "For server hosts", "For plugin developers")
SCOPE_AUDIENCE = {"server": "For server hosts", "ws-bridge": "For server hosts", "plugins": "For plugin developers"}
# Tooling and CI do not reach a player or a host (RELEASING.md, "When to release", rule 1): they are left out.
EXCLUDED_SCOPES = {"tools", "ci", "build", "test", "docs"}
CONVENTIONAL = re.compile(r"^(?P<type>feat|fix|perf)(?:\((?P<scope>[^)]*)\))?(?P<bang>!)?: (?P<text>.+)$")

NOT_TESTED = (
    "The full test suite (`just verify`) ran on each pull request before it merged. The tag's build did not repeat it "
    "(#1008): the build and packaging are what the tag proves. Each pull request body names the checks it ran."
)


def audience_of(scope: str | None) -> str:
    return SCOPE_AUDIENCE.get(scope or "", "For players")


def sections(subjects: list[str]) -> str:
    """The markdown section for the given commit subjects, newest first as git prints them."""
    by_audience: dict[str, list[str]] = {name: [] for name in AUDIENCES}
    for subject in subjects:
        match = CONVENTIONAL.match(subject)
        if match is None or match.group("scope") in EXCLUDED_SCOPES:
            continue
        prefix = "BREAKING: " if match.group("bang") else ""
        by_audience[audience_of(match.group("scope"))].append(f"- {prefix}{match.group('text')}")
    out = ["## What changed, by audience", ""]
    for name in AUDIENCES:
        out.append(f"### {name}")
        out.append("")
        out.extend(by_audience[name] or ["- Nothing for this audience in this release."])
        out.append("")
    out.extend(["## Not tested in this build", "", NOT_TESTED, ""])
    return "\n".join(out)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=".", help="the repository (default: the current directory)")
    parser.add_argument("--base", required=True, help="the older ref, excluded")
    parser.add_argument("--rev", required=True, help="the newer ref, included")
    args = parser.parse_args(argv)
    log = subprocess.run(
        ["git", "-C", args.repo, "log", "--no-merges", "--format=%s", f"{args.base}..{args.rev}"],
        check=True, capture_output=True, text=True,
    ).stdout
    sys.stdout.write(sections(log.splitlines()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
