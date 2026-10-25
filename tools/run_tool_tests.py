#!/usr/bin/env python3
"""Run the tools/tests suite and keep the run (#913).

A run that is only printed is lost when the terminal scrolls: a one-off failure then has a count
and no name. Every run therefore writes, under the log directory:

- `<stem>.log`, the stem being `<UTC stamp to the microsecond>-<pid>-<16 random hex>-<sha12>`: a
  header (UTC start, HEAD, tree clean or DIRTY, pytest's version and plugins, the
  pytest-randomly seed, the command), then pytest's full output, also echoed to stdout line by
  line, then an end line with the exit code;
- `<stem>.xml`: pytest's JUnit XML, one record per test;
- two JSON lines in `runs.jsonl`: `"event": "start"` before pytest starts (so a run that is
  killed outright still leaves its start), and `"event": "end"` after it exits, with the exit
  code, the summary line, the failing test ids (`classname::name` from the XML), whether the XML
  was read (`junit_state`: `ok`, `missing` or `unparseable`), the signal that interrupted the
  run, whether pytest's output was read to its end, and the paths of the two files.

pytest's short summary (on by default, `-rfE`) names every failure and error even under `-q`; the
log keeps it whole. When pytest-randomly is installed and neither the arguments nor
`$PYTEST_ADDOPTS` set a seed or disable it (`-p no:randomly`), the run draws a seed and passes it
as `--randomly-seed`, so an order-dependent failure can be replayed; a seed given there is
recorded as given.

Signals. pytest runs in its own session, so a terminal's CTRL+C or hangup reaches this script
alone. SIGINT, SIGTERM and SIGHUP are recorded, never fatal here: the first is passed to pytest's
process group as SIGINT (pytest stops and prints its summary); a second, or 30 s without an exit
($NEVR_TEST_INTERRUPT_GRACE_S), kills the group. When pytest has exited and a process it left
behind still holds its output open for 10 s ($NEVR_TEST_DRAIN_S), that group is killed and the
end record says so (`stray_killed`). A signal before pytest starts means pytest never starts. Either way the end
record is written. When stdout goes away (a closed pipe or terminal), the echo stops and the log
keeps the rest.

Exit code: pytest's. Exceptions, each loud on stderr:
- 2 before pytest runs, when the log directory or the start record cannot be written, or
  `--junitxml` is passed (the run owns its XML path): a run whose record cannot be kept is
  refused, never run unrecorded;
- 128 + the signal's number when a signal interrupted the run (130 for SIGINT);
- when the log or the end record cannot be written, pytest's exit code if it is non-zero, else 2:
  a run is never reported green without its record.

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
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

DEFAULT_LOG_DIR = "/var/tmp/work-nevr-runtime/test-runs/tools-tests"
# pytest's closing line: "1 failed, 1246 passed, 10 skipped in 224.38s", "3 xfailed, 2 warnings in 1.0s",
# "no tests ran in 0.01s", with or without "=" rules.
SUMMARY_RE = re.compile(
    r"(\b\d+ (passed|failed|errors?|skipped|deselected|xfailed|xpassed|warnings?)\b|\bno tests ran\b)"
    r".* in [0-9.]+s")
# Seconds pytest gets after the first signal ($NEVR_TEST_INTERRUPT_GRACE_S), and seconds its output may
# stay open after it exits, held by a process it left behind ($NEVR_TEST_DRAIN_S).
INTERRUPT_GRACE_S = float(os.environ.get("NEVR_TEST_INTERRUPT_GRACE_S") or 30)
DRAIN_S = float(os.environ.get("NEVR_TEST_DRAIN_S") or 10)
HANDLED_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)


def git(*args: str) -> str:
    out = subprocess.run(["git", *args], capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else ""


def run_stem(start: datetime.datetime, head: str) -> str:
    """A file stem no other run shares, even one started in the same microsecond by the same process."""
    return f"{start.strftime('%Y%m%dT%H%M%S.%fZ')}-{os.getpid()}-{secrets.token_hex(8)}-{head[:12]}"


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


def randomly_seed(args: list, addopts: str) -> tuple:
    """(extra args, seed) for pytest-randomly, from the arguments and $PYTEST_ADDOPTS.

    No plugin, or `-p no:randomly`: nothing added, seed None. A given `--randomly-seed`: nothing
    added, that value recorded (an integer, or the word given, such as `last`). Otherwise a drawn
    seed is added.
    """
    if importlib.util.find_spec("pytest_randomly") is None:
        return [], None
    tokens = shlex.split(addopts) + list(args)
    given = None
    for i, tok in enumerate(tokens):
        nxt = tokens[i + 1] if i + 1 < len(tokens) else ""
        if tok in ("-pno:randomly",) or (tok == "-p" and nxt == "no:randomly"):
            return [], None
        if tok.startswith("--randomly-seed="):
            given = tok.split("=", 1)[1]
        elif tok == "--randomly-seed":
            given = nxt
    if given is not None:
        return [], int(given) if given.isdigit() else given
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


class Run:
    """The pytest process, its output and the signals that arrive while it runs."""

    def __init__(self, log):
        self.log = log
        self.lock = threading.Lock()
        self.echo = True
        self.log_error = None
        self.summary = ""
        self.proc = None
        self.signals = []
        self.forwarded = 0
        self.first_signal_at = None
        self.stray_killed = False

    def emit(self, text: str) -> None:
        with self.lock:
            if self.echo:
                try:
                    sys.stdout.write(text)
                    sys.stdout.flush()
                except OSError:
                    # stdout is gone; the log keeps the run. Point fd 1 at /dev/null so the
                    # interpreter's own flush at exit cannot fail either.
                    self.echo = False
                    devnull = os.open(os.devnull, os.O_WRONLY)
                    os.dup2(devnull, sys.stdout.fileno())
                    os.close(devnull)
            if self.log_error is None:
                try:
                    self.log.write(text)
                    self.log.flush()
                except OSError as exc:
                    self.log_error = str(exc)
                    print(f"test-tools: FAIL cannot write the log: {exc}", file=sys.stderr, flush=True)

    def on_signal(self, signum, frame) -> None:
        self.signals.append(signal.Signals(signum).name)
        if self.first_signal_at is None:
            self.first_signal_at = time.monotonic()
        self.forward()

    def forward(self) -> None:
        """Pass each recorded signal on to pytest's group: SIGINT first, SIGKILL after."""
        if self.proc is None or self.proc.poll() is not None:
            return
        while self.forwarded < len(self.signals):
            self.kill_group(signal.SIGINT if self.forwarded == 0 else signal.SIGKILL)
            self.forwarded += 1

    def kill_group(self, sig) -> None:
        try:
            os.killpg(self.proc.pid, sig)
        except (ProcessLookupError, PermissionError):
            pass

    def pump(self) -> None:
        for line in self.proc.stdout:
            self.emit(line)
            if SUMMARY_RE.search(line):
                self.summary = line.strip().strip("= ")

    def wait(self) -> bool:
        """Wait for pytest and its output; True when the output was read to its end."""
        reader = threading.Thread(target=self.pump, daemon=True)
        reader.start()
        while True:
            try:
                self.proc.wait(timeout=0.2)
                break
            except subprocess.TimeoutExpired:
                if self.first_signal_at is not None and time.monotonic() - self.first_signal_at > INTERRUPT_GRACE_S:
                    self.kill_group(signal.SIGKILL)
        reader.join(DRAIN_S)
        if reader.is_alive():
            # pytest has exited and something it started still holds the pipe.
            self.stray_killed = True
            self.kill_group(signal.SIGKILL)
            reader.join(DRAIN_S)
        return not reader.is_alive()


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
    seed_args, seed = randomly_seed(args, os.environ.get("PYTEST_ADDOPTS", ""))
    cmd = [sys.executable, "-m", "pytest", f"--junitxml={xml_path}", *seed_args, *args]
    env = pytest_environment()
    base = {"start": start.isoformat(), "head": head, "tree": tree, "cmd": cmd, "pytest": env["pytest"],
            "plugins": env["plugins"], "randomly_seed": seed, "log": str(log_path), "junit": str(xml_path)}

    try:
        log = log_path.open("w", encoding="utf-8")
    except OSError as exc:
        print(f"test-tools: FAIL cannot write the log {log_path}: {exc}", file=sys.stderr)
        return 2
    run = Run(log)
    previous = {s: signal.signal(s, run.on_signal) for s in HANDLED_SIGNALS}
    output_complete = True
    try:
        if not append_record(index, {"event": "start", **base}):
            log.close()
            log_path.unlink()
            return 2
        for line in (f"test-tools: start {start.isoformat()}", f"test-tools: HEAD {head}",
                     f"test-tools: tree {tree}", f"test-tools: pytest {env['pytest']}",
                     f"test-tools: plugins {' '.join(f'{k}={v}' for k, v in env['plugins'].items())}",
                     f"test-tools: randomly-seed {seed}", f"test-tools: cmd {' '.join(cmd)}"):
            run.emit(line + "\n")
        if not run.signals:
            run.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                        encoding="utf-8", errors="replace", start_new_session=True)
            run.forward()
            output_complete = run.wait()
        pytest_rc = run.proc.returncode if run.proc else None
        if run.signals:
            rc = 128 + signal.Signals[run.signals[0]].value
        else:
            rc = pytest_rc
        end = datetime.datetime.now(datetime.timezone.utc)
        run.emit(f"test-tools: end {end.isoformat()} rc={rc}"
                 + (f" interrupted={run.signals[0]}" if run.signals else "")
                 + ("" if output_complete else " output=incomplete") + "\n")
    finally:
        for s, h in previous.items():
            signal.signal(s, h)
        log.close()

    failed, junit_state = failing_ids(xml_path)
    record = {"event": "end", **base, "end": end.isoformat(), "rc": rc, "pytest_rc": pytest_rc,
              "interrupted": bool(run.signals), "signals": run.signals, "summary": run.summary,
              "failed": failed, "junit_state": junit_state, "output_complete": output_complete,
              "stray_killed": run.stray_killed, "log_error": run.log_error}
    if not append_record(index, record) or run.log_error is not None:
        return rc if rc != 0 else 2
    if run.echo:
        print(f"test-tools: log {log_path}", flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
