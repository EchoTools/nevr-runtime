#!/usr/bin/env python3
"""Run the tools/tests suite and keep the run (#913).

A run that is only printed is lost when the terminal scrolls: a one-off failure then has a count
and no name. Every run therefore writes, under the log directory:

- `<stem>.log`, the stem being `<UTC stamp to the microsecond>-<pid>-<4 random hex>-<sha12>`: a
  header (UTC start, HEAD, tree clean or DIRTY, pytest's version and plugins, the
  pytest-randomly seed, the command), then pytest's full output, also echoed to stdout line by
  line, then an end line with the exit code;
- `<stem>.xml`: pytest's JUnit XML, one record per test;
- two JSON lines in `runs.jsonl`: `"event": "start"` before pytest starts (so a run that is
  killed outright still leaves its start), and `"event": "end"` after it exits, with the exit
  code, the summary line, the failing test ids, whether the XML was read (`junit_state`: `ok`,
  `missing` or `unparseable`) and the paths of the two files.

pytest's short summary (on by default, `-rfE`) names every failure and error even under `-q`; the
log keeps it whole. When pytest-randomly is installed and the arguments name no seed, the run
draws one and passes it as `--randomly-seed`, so an order-dependent failure can be replayed.

Exit code: pytest's. Exceptions, each loud on stderr:
- 2 before pytest runs, when the log directory cannot be written or `--junitxml` is passed (the
  run owns its XML path): a run whose record cannot be kept is refused, never run unrecorded;
- 130 on SIGINT or SIGTERM: pytest runs in its own session, so a terminal's CTRL+C reaches this
  script alone, which passes pytest one SIGINT, waits for it (kills it after 30 s) and writes the
  end record with `"interrupted": true`;
- when the end record cannot be appended, pytest's exit code if it is non-zero, else 2: a run is
  never reported green without its record.

Usage: run_tool_tests.py [pytest args...]   (default: -q tools/tests)
Log directory: $NEVR_TEST_LOG_DIR, else /var/tmp/work-nevr-runtime/test-runs/tools-tests.
"""
import datetime
import importlib.metadata
import importlib.util
import json
import os
import pathlib
import random
import re
import secrets
import signal
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

DEFAULT_LOG_DIR = "/var/tmp/work-nevr-runtime/test-runs/tools-tests"
# pytest's closing line: "1 failed, 1246 passed, 10 skipped in 224.38s", "3 xfailed, 2 warnings in 1.0s",
# "no tests ran in 0.01s", with or without "=" rules.
SUMMARY_RE = re.compile(
    r"(\b\d+ (passed|failed|errors?|skipped|deselected|xfailed|xpassed|warnings?)\b|\bno tests ran\b)"
    r".* in [0-9.]+s")
INTERRUPT_GRACE_S = 30


class Interrupted(Exception):
    """SIGINT or SIGTERM arrived while pytest ran."""


def git(*args: str) -> str:
    out = subprocess.run(["git", *args], capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else ""


def run_stem(start: datetime.datetime, head: str) -> str:
    """A file stem no other run shares, even one started in the same microsecond by the same process."""
    return f"{start.strftime('%Y%m%dT%H%M%S.%fZ')}-{os.getpid()}-{secrets.token_hex(2)}-{head[:12]}"


def failing_ids(xml_path: pathlib.Path) -> tuple:
    """(the `classname::name` of every failed or errored test case, junit_state)."""
    if not xml_path.exists():
        return [], "missing"
    try:
        root = ET.parse(xml_path).getroot()
    except ET.ParseError:
        return [], "unparseable"
    ids = []
    for case in root.iter("testcase"):
        if case.find("failure") is not None or case.find("error") is not None:
            ids.append(f"{case.get('classname', '')}::{case.get('name', '')}")
    return ids, "ok"


def pytest_environment() -> dict:
    """pytest's version and the installed pytest plugins (entry-point name -> distribution and version)."""
    plugins = {}
    for ep in importlib.metadata.entry_points(group="pytest11"):
        dist = ep.dist
        plugins[ep.name] = f"{dist.name} {dist.version}" if dist else "unknown"
    try:
        version = importlib.metadata.version("pytest")
    except importlib.metadata.PackageNotFoundError:
        version = "not installed"
    return {"pytest": version, "plugins": dict(sorted(plugins.items()))}


def randomly_seed(args: list) -> tuple:
    """(extra args, seed): draw a seed when pytest-randomly is installed and the args don't set or disable it."""
    if importlib.util.find_spec("pytest_randomly") is None:
        return [], None
    if any("randomly" in a for a in args):
        return [], None
    seed = random.SystemRandom().randrange(2**32)
    return [f"--randomly-seed={seed}"], seed


def append_record(index: pathlib.Path, record: dict) -> bool:
    try:
        with index.open("a", encoding="utf-8") as f:
            f.write(json.dumps(record) + "\n")
        return True
    except OSError as exc:
        print(f"test-tools: FAIL cannot append the {record['event']} record to {index}: {exc}",
              file=sys.stderr, flush=True)
        return False


def raise_interrupted(signum, frame):
    raise Interrupted(signal.Signals(signum).name)


def main(argv: list) -> int:
    if any(a == "--junitxml" or a.startswith("--junitxml=") or a.startswith("--junit-xml") for a in argv):
        print("test-tools: FAIL --junitxml is not accepted: the run writes its own XML beside its log",
              file=sys.stderr)
        return 2

    log_dir = pathlib.Path(os.environ.get("NEVR_TEST_LOG_DIR") or DEFAULT_LOG_DIR)
    try:
        log_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=log_dir, prefix=".writable-"):
            pass
    except OSError as exc:
        print(f"test-tools: FAIL cannot write the run record to {log_dir}: {exc}", file=sys.stderr)
        return 2

    head = git("rev-parse", "HEAD") or "unknown"
    tree = "DIRTY" if git("status", "--porcelain") else "clean"
    start = datetime.datetime.now(datetime.timezone.utc)
    stem = run_stem(start, head)
    log_path = log_dir / f"{stem}.log"
    xml_path = log_dir / f"{stem}.xml"
    index = log_dir / "runs.jsonl"
    args = argv or ["-q", "tools/tests"]
    seed_args, seed = randomly_seed(args)
    cmd = [sys.executable, "-m", "pytest", f"--junitxml={xml_path}", *seed_args, *args]
    env = pytest_environment()
    base = {"start": start.isoformat(), "head": head, "tree": tree, "cmd": cmd, "pytest": env["pytest"],
            "plugins": env["plugins"], "randomly_seed": seed, "log": str(log_path), "junit": str(xml_path)}
    if not append_record(index, {"event": "start", **base}):
        return 2

    summary = ""
    rc = None
    interrupted = None
    previous = {s: signal.signal(s, raise_interrupted) for s in (signal.SIGINT, signal.SIGTERM)}
    with log_path.open("w", encoding="utf-8") as log:
        def emit(line: str) -> None:
            sys.stdout.write(line)
            sys.stdout.flush()
            log.write(line)
            log.flush()

        for line in (f"test-tools: start {start.isoformat()}", f"test-tools: HEAD {head}",
                     f"test-tools: tree {tree}", f"test-tools: pytest {env['pytest']}",
                     f"test-tools: plugins {' '.join(f'{k}={v}' for k, v in env['plugins'].items())}",
                     f"test-tools: randomly-seed {seed}", f"test-tools: cmd {' '.join(cmd)}"):
            emit(line + "\n")
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                encoding="utf-8", errors="replace", start_new_session=True)
        try:
            try:
                for line in proc.stdout:
                    emit(line)
                    if SUMMARY_RE.search(line):
                        summary = line.strip().strip("= ")
                rc = proc.wait()
            except Interrupted as exc:
                interrupted = str(exc)
                for s, h in previous.items():
                    signal.signal(s, h)
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=INTERRUPT_GRACE_S)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                rest = proc.stdout.read()
                if rest:
                    emit(rest)
                rc = 130
        finally:
            for s, h in previous.items():
                signal.signal(s, h)
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            if rc is None:
                rc = proc.returncode if proc.returncode is not None else 1
            end = datetime.datetime.now(datetime.timezone.utc)
            emit(f"test-tools: end {end.isoformat()} rc={rc}"
                 + (f" interrupted={interrupted}" if interrupted else "") + "\n")

    failed, junit_state = failing_ids(xml_path)
    record = {"event": "end", **base, "end": end.isoformat(), "rc": rc, "pytest_rc": proc.returncode,
              "interrupted": interrupted is not None, "summary": summary, "failed": failed,
              "junit_state": junit_state}
    if not append_record(index, record):
        return rc if rc != 0 else 2
    print(f"test-tools: log {log_path}", flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
