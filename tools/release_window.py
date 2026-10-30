#!/usr/bin/env python3
"""The release window (#1466): no release is cut or dispatched within 195 minutes of the newest published release.

One rule, shared by the scheduled release train (release-train.yml, the "Check the release window" step, which runs this
file as a command) and the train guard (tools/release_train_guard.py, which imports it). The newest published release is
the newest, by `publishedAt`, of the repository's non-draft releases whose tag is exactly vX.Y.Z; a draft, a tag of any
other shape and the order `gh release list` returns them in do not count.

    python3 tools/release_window.py --repo OWNER/REPO [--gh GH] [--now ISO]

prints one JSON line and exits 0 when the train may go on ("clear": no such release, or it is older than the window) or
10 when it holds ("hold": it was published 195.0 minutes ago or less). The line is also appended to $GITHUB_STEP_SUMMARY
when that variable is set. Any failure (gh missing or failing, JSON that does not parse, a published release with no
readable publishedAt) exits 2 and names the call on stderr; nothing is swallowed. <gh> is --gh, else $GH_BIN, else gh.
--now exists for tests. Standard library only.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import subprocess
import sys

TOOL = "release_window"
WINDOW_MINUTES = 195
CLEAR_EXIT = 0
HOLD_EXIT = 10
FAILURE_EXIT = 2
DEFAULT_GH = "gh"
RELEASE_FIELDS = "tagName,isDraft,isPrerelease,publishedAt"
RELEASE_TAG = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")


class WindowError(Exception):
    pass


def parse_time(stamp: str) -> dt.datetime:
    return dt.datetime.fromisoformat(stamp.replace("Z", "+00:00"))


def age_minutes(now: dt.datetime, stamp: str) -> float:
    return round((now - parse_time(stamp)).total_seconds() / 60, 1)


def inside_window(now: dt.datetime, stamp: str) -> bool:
    """True when `stamp` is WINDOW_MINUTES old or less. The age is rounded to one decimal first, as it is logged."""
    return age_minutes(now, stamp) <= WINDOW_MINUTES


def list_args(repo: str) -> list[str]:
    """The gh arguments that list the releases the rule reads."""
    return ["release", "list", "--repo", repo, "--limit", "20", "--json", RELEASE_FIELDS]


def newest_published(listing: str) -> dict | None:
    """The newest published vX.Y.Z release in `gh release list --json ...` output, as {"tag", "published"}; None when
    there is none. The newest is by publishedAt, not by position. Raises WindowError for output it cannot read."""
    try:
        releases = json.loads(listing)
    except json.JSONDecodeError as error:
        raise WindowError(f"release list printed JSON that does not parse: {error}") from error
    if not isinstance(releases, list):
        raise WindowError("release list printed JSON that is not a list")
    newest: dict | None = None
    newest_time: dt.datetime | None = None
    for item in releases:
        if not isinstance(item, dict):
            raise WindowError("release list printed an entry that is not an object")
        tag = item.get("tagName")
        if item.get("isDraft") is True or not isinstance(tag, str) or not RELEASE_TAG.match(tag):
            continue
        stamp = item.get("publishedAt")
        try:
            published = parse_time(stamp)
        except (AttributeError, TypeError, ValueError) as error:
            raise WindowError(f"release {tag} has no readable publishedAt: {stamp!r}") from error
        if published.tzinfo is None:
            raise WindowError(f"release {tag} has a publishedAt with no time zone: {stamp!r}")
        if newest_time is None or published > newest_time:
            newest, newest_time = {"tag": tag, "published": stamp}, published
    return newest


def decision(now: dt.datetime, release: dict | None) -> dict:
    """The CLI's decision for the newest published release (None: there is none)."""
    held = release is not None and inside_window(now, release["published"])
    return {
        "tool": TOOL,
        "event": "decision",
        "action": "hold" if held else "clear",
        "reason": "release_window" if held else "no_recent_release",
        "last_release": release["tag"] if release else None,
        "last_published": release["published"] if release else None,
        "age_minutes": age_minutes(now, release["published"]) if release else None,
        "window_minutes": WINDOW_MINUTES,
    }


def list_releases(repo: str, gh: str) -> str:
    """Run `gh release list` for the rule. A failure is named, never swallowed."""
    args = list_args(repo)
    name = os.path.basename(gh)
    try:
        proc = subprocess.run([gh, *args], capture_output=True, text=True)
    except OSError as error:
        raise WindowError(f"{name} release list could not run: {error.strerror or error}") from error
    if proc.returncode != 0:
        raise WindowError(f"{name} release list failed with exit {proc.returncode}: {proc.stderr.strip()}")
    return proc.stdout


def append_summary(line: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as summary:
        summary.write(line + "\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""), help="owner/name")
    parser.add_argument("--gh", default=None, help="the gh binary (default: $GH_BIN, else gh)")
    parser.add_argument("--now", default=None, help="the time to measure from, ISO 8601 (tests; default: the clock)")
    args = parser.parse_args(argv)
    if not args.repo:
        sys.stderr.write("release_window: no repository: pass --repo owner/name or set GITHUB_REPOSITORY\n")
        return FAILURE_EXIT
    gh = args.gh or os.environ.get("GH_BIN") or DEFAULT_GH
    try:
        now = parse_time(args.now) if args.now else dt.datetime.now(dt.timezone.utc)
        if now.tzinfo is None:
            raise WindowError(f"--now has no time zone: {args.now!r}")
        release = newest_published(list_releases(args.repo, gh))
        record = decision(now, release)
        line = json.dumps(record)
        print(line, flush=True)
        append_summary(line)
    except (WindowError, ValueError, OSError) as failure:
        sys.stderr.write(json.dumps({"tool": TOOL, "event": "fail", "reason": str(failure)}) + "\n")
        return FAILURE_EXIT
    return HOLD_EXIT if record["action"] == "hold" else CLEAR_EXIT


if __name__ == "__main__":
    sys.exit(main())
