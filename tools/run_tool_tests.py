#!/usr/bin/env python3
"""Run the tools/tests suite and keep the run (#913).

A run that is only printed is lost when the terminal scrolls: a one-off failure then has a count
and no name. Every run therefore writes, under the log directory:

- `<stamp>-<sha12>.log`: a header (UTC start, HEAD, tree clean or dirty, the command) and
  pytest's full output, also echoed to stdout as it comes;
- `<stamp>-<sha12>.xml`: pytest's JUnit XML, one record per test;
- one JSON line in `runs.jsonl`: start, end, head, tree, command, exit code, the summary line,
  the failing test ids and the paths of the two files.

pytest's short summary (on by default, `-rfE`) names every failure and error even under `-q`; the
log keeps it whole.
The exit code is pytest's. A log directory that cannot be written is exit 2 with a message, before
pytest runs: a run whose record cannot be kept is refused, never run unrecorded.

Usage: run_tool_tests.py [pytest args...]   (default: -q tools/tests)
Log directory: $NEVR_TEST_LOG_DIR, else /var/tmp/work-nevr-runtime/test-runs/tools-tests.
"""
import datetime
import json
import os
import pathlib
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

DEFAULT_LOG_DIR = "/var/tmp/work-nevr-runtime/test-runs/tools-tests"
# pytest's closing count line: "1 failed, 1246 passed, 10 skipped in 224.38s", with or without "=" rules.
SUMMARY_RE = re.compile(r"\b\d+ (passed|failed|errors?|skipped|deselected)\b.* in [0-9.]+s")


def git(*args: str) -> str:
    out = subprocess.run(["git", *args], capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else ""


def failing_ids(xml_path: pathlib.Path) -> list:
    """The `classname::name` of every failed or errored test case in a JUnit XML file."""
    if not xml_path.exists():
        return []
    ids = []
    for case in ET.parse(xml_path).getroot().iter("testcase"):
        if case.find("failure") is not None or case.find("error") is not None:
            ids.append(f"{case.get('classname', '')}::{case.get('name', '')}")
    return ids


def main(argv: list) -> int:
    log_dir = pathlib.Path(os.environ.get("NEVR_TEST_LOG_DIR") or DEFAULT_LOG_DIR)
    try:
        log_dir.mkdir(parents=True, exist_ok=True)
        probe = log_dir / ".writable"
        probe.write_text("")
        probe.unlink()
    except OSError as exc:
        print(f"test-tools: FAIL cannot write the run record to {log_dir}: {exc}", file=sys.stderr)
        return 2

    head = git("rev-parse", "HEAD") or "unknown"
    tree = "DIRTY" if git("status", "--porcelain") else "clean"
    start = datetime.datetime.now(datetime.timezone.utc)
    stem = f"{start.strftime('%Y%m%dT%H%M%SZ')}-{head[:12]}"
    log_path = log_dir / f"{stem}.log"
    xml_path = log_dir / f"{stem}.xml"
    args = argv or ["-q", "tools/tests"]
    cmd = [sys.executable, "-m", "pytest", f"--junitxml={xml_path}", *args]

    summary = ""
    with log_path.open("w", encoding="utf-8") as log:
        header = [f"test-tools: start {start.isoformat()}", f"test-tools: HEAD {head}",
                  f"test-tools: tree {tree}", f"test-tools: cmd {' '.join(cmd)}"]
        for line in header:
            print(line)
            log.write(line + "\n")
        log.flush()
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in proc.stdout:
            sys.stdout.write(line)
            log.write(line)
            if SUMMARY_RE.search(line):
                summary = line.strip().strip("= ")
        rc = proc.wait()
        end = datetime.datetime.now(datetime.timezone.utc)
        tail = f"test-tools: end {end.isoformat()} rc={rc}"
        print(tail)
        log.write(tail + "\n")

    record = {"start": start.isoformat(), "end": end.isoformat(), "head": head, "tree": tree,
              "cmd": cmd, "rc": rc, "summary": summary, "failed": failing_ids(xml_path),
              "log": str(log_path), "junit": str(xml_path)}
    try:
        with (log_dir / "runs.jsonl").open("a", encoding="utf-8") as index:
            index.write(json.dumps(record) + "\n")
    except OSError as exc:
        print(f"test-tools: FAIL cannot append to {log_dir / 'runs.jsonl'}: {exc}", file=sys.stderr)
        return 2
    print(f"test-tools: log {log_path}")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
