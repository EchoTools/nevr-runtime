#!/usr/bin/env python3
"""Dispatch the release train when a scheduled run was dropped (#1409).

release-train.yml runs every three hours (cron "17 */3 * * *"), and GitHub has dropped a scheduled slot. This guard is meant
to run hourly from a scheduler (cron, a systemd timer) on a maintainer's machine, not as a GitHub
schedule: GitHub drops scheduled runs, and a job's token cannot read repository variables. It decides from the train's
run list and the newest published release (tools/release_window.py) whether the train is due:

- switch off (repository variable RELEASE_TRAIN is not "true")        -> none, switch_off
- nothing releasable on origin/main (next_release_version.py exit 1)   -> none, no_releasable_change
- a train run queued, in progress, waiting or pending                   -> none, train_run_in_progress
- the newest train run failed, was cancelled or timed out, and was
  created 195 minutes ago or less                                       -> none, recent_failed_run
- the newest published release is 195 minutes old or less               -> none, recent_release
- otherwise                                                             -> dispatch, no_release_in_window

No published vX.Y.Z release among the releases fetched is not "no release in the window": the guard cannot see the last
release, so it stops with a named failure (non-zero exit, a fail line in the log) and dispatches nothing.

The window is measured from the newest published release, not from the newest train run (#1466): a run that held, or
found nothing to cut, published nothing and costs no time. A run that failed red (or was cancelled or timed out) is the
exception: it may have left a tag and an unpublished draft, which the version tool then counts as the base, so a guard
that only watched the release would dispatch again every hour while merges keep landing. The newest run's conclusion
(failure, cancelled, timed_out) inside the same window therefore holds the guard (recent_failed_run). A run that
succeeded or was skipped does not. The rule is tools/release_window.py, which the scheduled
train runs too; "newest published release" is its newest_published (non-draft, tag exactly vX.Y.Z, newest by
publishedAt), read through this guard's own gh call. A decision carries last_run_id and last_run_created (the newest
train run, information only) and last_release, last_published, age_minutes (of that release) and window_minutes.

Every decision is one JSON line on stdout and is appended to the log file (--log-file, default
$XDG_STATE_HOME/nevr-runtime/release-train-guard.jsonl, else ~/.local/state/nevr-runtime/...). It is also appended to
$GITHUB_STEP_SUMMARY when that variable is set. A dispatch is `<gh> workflow run release-train.yml --ref main`, followed by
a {"event": "dispatched"} line. <gh> is --gh, else $GH_BIN, else gh. A log that cannot be written before the run
starts fails the run before any gh call; any failed gh or tool call exits non-zero and names the call. Standard library
only; the fakes in tools/tests/test_release_train_guard.py drive main() end to end.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import subprocess
import sys

# `python3 -I` puts neither this folder nor the cwd on sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import release_window  # noqa: E402

WORKFLOW = "release-train.yml"
WINDOW_MINUTES = release_window.WINDOW_MINUTES
ACTIVE = frozenset({"queued", "in_progress", "waiting", "pending"})
FAILED = frozenset({"failure", "cancelled", "timed_out"})
TOOL = "release_train_guard"
DEFAULT_GH = "gh"
LOG_NAME = "release-train-guard.jsonl"
VERSION_TOOL = pathlib.Path(__file__).resolve().with_name("next_release_version.py")


class GuardFailure(Exception):
    pass


def decide(now: dt.datetime, runs: list[dict], releasable: bool, switch_on: bool, release: dict) -> dict:
    """The decision for one hourly tick. `runs` is the train's runs, newest first; `now` is an aware datetime;
    `release` is the newest published release, {"tag", "published"} (release_window.newest_published)."""
    newest = runs[0] if runs else None
    last_id = newest["id"] if newest else None
    last_created = newest["createdAt"] if newest else None
    age = release_window.age_minutes(now, release["published"])

    def result(action: str, reason: str) -> dict:
        return {
            "action": action,
            "reason": reason,
            "last_run_id": last_id,
            "last_run_created": last_created,
            "last_release": release["tag"],
            "last_published": release["published"],
            "age_minutes": age,
            "window_minutes": WINDOW_MINUTES,
        }

    if not switch_on:
        return result("none", "switch_off")
    if not releasable:
        return result("none", "no_releasable_change")
    if any(run["status"] in ACTIVE for run in runs):
        return result("none", "train_run_in_progress")
    if (
        newest is not None
        and newest.get("conclusion") in FAILED
        and release_window.inside_window(now, newest["createdAt"])
    ):
        return result("none", "recent_failed_run")
    if release_window.inside_window(now, release["published"]):
        return result("none", "recent_release")
    return result("dispatch", "no_release_in_window")


def gh_binary(env: dict[str, str]) -> str:
    """The gh the guard calls when --gh is not given: $GH_BIN, else gh (as the Quest timer)."""
    return env.get("GH_BIN") or DEFAULT_GH


def default_log_path(env: dict[str, str]) -> pathlib.Path:
    state = env.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return pathlib.Path(state) / "nevr-runtime" / LOG_NAME


class Call:
    """One gh call, run with the token from the environment. A failure is named, never swallowed."""

    def __init__(self, gh: str, args: list[str]) -> None:
        self.gh = gh
        self.args = args
        self.what = " ".join(args[:3])

    def stdout(self) -> str:
        name = pathlib.Path(self.gh).name
        try:
            proc = subprocess.run([self.gh, *self.args], capture_output=True, text=True)
        except OSError as error:
            raise GuardFailure(f"{name} {self.what} could not run: {error.strerror or error}") from error
        if proc.returncode != 0:
            raise GuardFailure(f"{name} {self.what} failed with exit {proc.returncode}: {proc.stderr.strip()}")
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


def switch_is_on(repo: str, gh: str) -> bool:
    variables = json.loads(Call(gh, ["variable", "list", "--repo", repo, "--json", "name,value"]).stdout())
    return any(v["name"] == "RELEASE_TRAIN" and v["value"] == "true" for v in variables)


def train_runs(repo: str, gh: str) -> list[dict]:
    listed = Call(gh, [
        "run", "list", "--workflow", WORKFLOW, "--repo", repo, "--limit", "20",
        "--json", "databaseId,status,event,createdAt,conclusion",
    ]).stdout()
    return [
        {
            "id": r["databaseId"],
            "createdAt": r["createdAt"],
            "status": r["status"],
            "event": r["event"],
            "conclusion": r.get("conclusion"),
        }
        for r in json.loads(listed)
    ]


def newest_release(repo: str, gh: str) -> dict:
    """The newest published release, by the shared rule. The call is the guard's own, so its failures are the guard's."""
    listing = Call(gh, release_window.list_args(repo)).stdout()
    try:
        return release_window.newest_published(listing)
    except release_window.WindowError as error:
        raise GuardFailure(f"gh release list {repo}: {error}") from error


def ensure_log(path: pathlib.Path) -> None:
    """Create the log and its folder now, so a run that cannot record its decision stops before it decides anything."""
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a", encoding="utf-8"):
            pass
    except OSError as error:
        raise GuardFailure(f"cannot write the log {path}: {error.strerror or error}") from error


def append_log(path: pathlib.Path, record: dict) -> None:
    try:
        with open(path, "a", encoding="utf-8") as log:
            log.write(json.dumps(record) + "\n")
    except OSError as error:
        raise GuardFailure(f"cannot write the log {path}: {error.strerror or error}") from error


def emit(record: dict, log_path: pathlib.Path) -> None:
    """Append to the log first: a line is printed only once it is on record."""
    line = {"tool": TOOL, **record}
    append_log(log_path, line)
    print(json.dumps(line), flush=True)


def append_summary(line: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as summary:
        summary.write(line + "\n")


def report_failure(reason: str, log_path: pathlib.Path) -> int:
    fail = {"tool": TOOL, "event": "fail", "reason": reason}
    sys.stderr.write(json.dumps(fail) + "\n")
    try:
        append_log(log_path, fail)
    except GuardFailure as log_failure:
        sys.stderr.write(f"release_train_guard: {log_failure}\n")
    return 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""), help="owner/name")
    parser.add_argument("--dry-run", action="store_true", help="print the decision and do not dispatch")
    parser.add_argument("--version-tool", type=pathlib.Path, default=VERSION_TOOL,
                        help="next_release_version.py (default: the one beside this file)")
    parser.add_argument("--gh", default=None, help="the gh binary (default: $GH_BIN, else gh)")
    parser.add_argument("--log-file", type=pathlib.Path, default=None,
                        help="the JSONL decision log (default: $XDG_STATE_HOME/nevr-runtime/release-train-guard.jsonl)")
    args = parser.parse_args(argv)
    if not args.repo:
        sys.stderr.write("release_train_guard: no repository: pass --repo owner/name or set GITHUB_REPOSITORY\n")
        return 1
    gh = args.gh or gh_binary(os.environ)
    log_path = args.log_file or default_log_path(os.environ)
    try:
        ensure_log(log_path)
    except GuardFailure as failure:
        return report_failure(str(failure), log_path)
    now = dt.datetime.now(dt.timezone.utc)
    try:
        tag = releasable_tag(args.repo, args.version_tool)
        switch_on = switch_is_on(args.repo, gh)
        runs = train_runs(args.repo, gh)
        release = newest_release(args.repo, gh)
        decision = decide(now, runs, tag is not None, switch_on, release)
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
        emit(record, log_path)
        append_summary(json.dumps({"tool": TOOL, **record}))
        if decision["action"] == "dispatch" and not args.dry_run:
            Call(gh, ["workflow", "run", WORKFLOW, "--repo", args.repo, "--ref", "main"]).stdout()
            emit({"event": "dispatched"}, log_path)
    except GuardFailure as failure:
        return report_failure(str(failure), log_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
