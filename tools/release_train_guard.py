#!/usr/bin/env python3
"""Dispatch the release train when a scheduled run was dropped (#1409).

release-train.yml runs every three hours (cron "17 */3 * * *"), and GitHub has dropped a scheduled slot. This guard runs
hourly (release-train-guard.yml) and decides from the train's own run list whether the train is due:

- switch off (repository variable RELEASE_TRAIN is not "true")        -> none, switch_off
- nothing releasable on origin/main (next_release_version.py exit 1)   -> none, no_releasable_change
- a train run queued, in progress, waiting or pending                   -> none, train_run_in_progress
- the newest train run created within 195 minutes of now                -> none, recent_run
- otherwise (also no run at all)                                        -> dispatch, no_run_in_window

Every decision is one JSON line on stdout and is appended to $GITHUB_STEP_SUMMARY. A dispatch is `gh workflow run
release-train.yml --ref main`, followed by a second line {"event": "dispatched"}. Any failed gh or tool call exits non-zero
and names the call. Standard library only; the fakes in tools/tests/test_release_train_guard.py drive main() end to end.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import subprocess
import sys

WORKFLOW = "release-train.yml"
WINDOW_MINUTES = 195
ACTIVE = frozenset({"queued", "in_progress", "waiting", "pending"})
TOOL = "release_train_guard"
VERSION_TOOL = pathlib.Path(__file__).resolve().with_name("next_release_version.py")


class GuardFailure(Exception):
    pass


def parse_time(stamp: str) -> dt.datetime:
    return dt.datetime.fromisoformat(stamp.replace("Z", "+00:00"))


def decide(now: dt.datetime, runs: list[dict], releasable: bool, switch_on: bool) -> dict:
    """The decision for one hourly tick. `runs` is the train's runs, newest first; `now` is an aware datetime."""
    newest = runs[0] if runs else None
    last_id = newest["id"] if newest else None
    last_created = newest["createdAt"] if newest else None
    age = None
    if newest is not None:
        age = round((now - parse_time(newest["createdAt"])).total_seconds() / 60, 1)

    def result(action: str, reason: str) -> dict:
        return {
            "action": action,
            "reason": reason,
            "last_run_id": last_id,
            "last_run_created": last_created,
            "age_minutes": age,
        }

    if not switch_on:
        return result("none", "switch_off")
    if not releasable:
        return result("none", "no_releasable_change")
    if any(run["status"] in ACTIVE for run in runs):
        return result("none", "train_run_in_progress")
    if newest is not None and age <= WINDOW_MINUTES:
        return result("none", "recent_run")
    return result("dispatch", "no_run_in_window")


class Call:
    """One gh call, run with the token from the environment. A failure is named, never swallowed."""

    def __init__(self, args: list[str]) -> None:
        self.args = args
        self.what = " ".join(args[:3])

    def stdout(self) -> str:
        proc = subprocess.run(["gh", *self.args], capture_output=True, text=True)
        if proc.returncode != 0:
            raise GuardFailure(f"gh {self.what} failed with exit {proc.returncode}: {proc.stderr.strip()}")
        return proc.stdout


def releasable_tag(repo: str, version_tool: pathlib.Path) -> str | None:
    proc = subprocess.run(
        [sys.executable, str(version_tool), "--repo", ".", "--rev", "origin/main"],
        capture_output=True,
        text=True,
    )
    sys.stderr.write(proc.stderr)
    if proc.returncode == 0:
        return proc.stdout.strip()
    if proc.returncode == 1:
        return None
    raise GuardFailure(f"next_release_version.py failed with exit {proc.returncode} on {repo}")


def switch_is_on(repo: str) -> bool:
    variables = json.loads(Call(["variable", "list", "--repo", repo, "--json", "name,value"]).stdout())
    return any(v["name"] == "RELEASE_TRAIN" and v["value"] == "true" for v in variables)


def train_runs(repo: str) -> list[dict]:
    listed = Call([
        "run", "list", "--workflow", WORKFLOW, "--repo", repo, "--limit", "20",
        "--json", "databaseId,status,event,createdAt",
    ]).stdout()
    return [
        {"id": r["databaseId"], "createdAt": r["createdAt"], "status": r["status"], "event": r["event"]}
        for r in json.loads(listed)
    ]


def emit(record: dict) -> None:
    print(json.dumps({"tool": TOOL, **record}), flush=True)


def append_summary(line: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as summary:
        summary.write(line + "\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""), help="owner/name")
    parser.add_argument("--dry-run", action="store_true", help="print the decision and do not dispatch")
    parser.add_argument("--version-tool", type=pathlib.Path, default=VERSION_TOOL,
                        help="next_release_version.py (default: the one beside this file)")
    args = parser.parse_args(argv)
    if not args.repo:
        sys.stderr.write("release_train_guard: no repository: pass --repo owner/name or set GITHUB_REPOSITORY\n")
        return 1
    now = dt.datetime.now(dt.timezone.utc)
    try:
        tag = releasable_tag(args.repo, args.version_tool)
        switch_on = switch_is_on(args.repo)
        runs = train_runs(args.repo)
        decision = decide(now, runs, tag is not None, switch_on)
        record = {
            "event": "decision",
            **decision,
            "releasable": tag is not None,
            "tag": tag,
            "switch_on": switch_on,
            "repo": args.repo,
            "dry_run": args.dry_run,
            "ts": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        }
        emit(record)
        append_summary(json.dumps({"tool": TOOL, **record}))
        if decision["action"] == "dispatch" and not args.dry_run:
            Call(["workflow", "run", WORKFLOW, "--repo", args.repo, "--ref", "main"]).stdout()
            emit({"event": "dispatched"})
    except GuardFailure as failure:
        sys.stderr.write(json.dumps({"tool": TOOL, "event": "fail", "reason": str(failure)}) + "\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
