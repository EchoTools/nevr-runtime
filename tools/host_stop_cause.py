#!/usr/bin/env python3
"""Say why the host stopped the game, from the host's own record (#1122).

    tools/host_stop_cause.py --log <nevr-bootstrap.jsonl> --since <unix seconds>

A run whose host stops the game (nevr-runtime.dll absent or refused) leaves no game log, because the runtime
writes that log and never ran. The host's record, `nevr-bootstrap.jsonl` in the nevr root's logs folder, holds the
cause: a `runtime_load` record with the outcome and the verifier's reason, then a `game_stop` record. This prints
one line naming the cause for the newest stop written at or after --since, and exits 0. It exits 3 and prints
nothing when the record is absent, unreadable, or holds no stop since then, so the caller keeps its own message.
"""
import argparse
import datetime
import json
import pathlib
import sys

NO_STOP = 3

# What the developer does about a refusal the build can cause. The reason words are the verifier's own
# (src/runtime/host/runtime_load.cpp, `verify_reason`).
UNSIGNED_ADVICE = ("a build you run yourself must be the plugin developers' build, which the host lets load unsigned: "
                   "build with mingw-plugin-dev or mingw-scenario (NEVR_ALLOW_UNSIGNED_PLUGINS)")


def stamp(record: dict):
    """The record's time as unix seconds, or None when it has none the reading can use."""
    try:
        return datetime.datetime.fromisoformat(record["ts"]).timestamp()
    except (KeyError, TypeError, ValueError):
        return None


def records(path: pathlib.Path) -> list:
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            record = json.loads(line)
        except ValueError:
            continue  # a torn line is not a record; the lines around it still are
        if isinstance(record, dict):
            out.append(record)
    return out


def cause(log: pathlib.Path, since: float):
    """The sentence for the newest game_stop at or after `since`, or None."""
    try:
        found = records(log)
    except OSError:
        return None
    stops = [r for r in found if r.get("event") == "game_stop" and (stamp(r) or 0) >= since]
    if not stops:
        return None
    stop = stops[-1]
    load = None
    for record in found:
        if record.get("event") == "runtime_load" and record.get("run") == stop.get("run") and record.get("outcome") != "loaded":
            load = record
    head = f"the host stopped the game (exit {stop.get('exit_code', '?')}, reason {stop.get('reason', '?')})"
    path = (load or stop).get("path") or stop.get("runtime_path") or "nevr-runtime.dll"
    if load is not None and load.get("outcome") == "runtime_refused":
        reason = load.get("verify_reason") or load.get("verify") or "refused"
        text = f"{head}: the host refused nevr-runtime.dll: {reason} ({path})"
        if reason == "unsigned":
            text += f"; {UNSIGNED_ADVICE}"
        return text
    if load is not None and load.get("outcome") == "runtime_absent":
        return f"{head}: nevr-runtime.dll is not beside BugSplat64.dll ({path})"
    if load is not None:
        return f"{head}: runtime_load outcome {load.get('outcome')}"
    return head


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--log", required=True, type=pathlib.Path)
    parser.add_argument("--since", required=True, type=float)
    args = parser.parse_args()
    text = cause(args.log, args.since)
    if text is None:
        return NO_STOP
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
