#!/usr/bin/env python3
"""Run the tools/tests suite and keep the run (#913).

A run that is only printed is lost when the terminal scrolls: a one-off failure then has a count
and no name. Every run therefore writes, under the log directory:

- `<stem>.log`, the stem being `<UTC stamp to the microsecond>-<pid>-<16 random hex>-<sha12>`: a
  header (UTC start, HEAD, tree clean, DIRTY or unknown, pytest's version and plugins, the
  pytest-randomly seed, the command), then pytest's full output, also echoed to stdout line by
  line, then an end line with the exit code;
- `<stem>.xml`: pytest's JUnit XML, one record per test;
- two JSON lines in `runs.jsonl`: `"event": "start"` before pytest starts (so a run that is killed
  outright still leaves its start), and `"event": "end"` after it exits, with the exit code, the
  summary line, the failing test ids (`classname::name` from the XML, or the name alone when the
  XML gives no class, as for a collection error), whether the XML was read (`junit_state`: `ok`,
  `missing`, `unparseable` or `unreadable`), the signals received, whether pytest's output was read
  to its end (`output_complete`), whether a process pytest left behind was sent a kill
  (`stray_kill_sent`), what became of the echoes (`echo` for stdout, `stderr_echo`), any error
  writing the log, reading the output or starting pytest, and the paths of the two files. Both
  records name the checkout the run was started in (`cwd`) and the user who ran it (`user`).

pytest's short summary (on by default, `-rfE`) names every failure and error even under `-q`; the
log keeps it whole. When pytest-randomly is installed and loads automatically, and neither the
arguments nor `$PYTEST_ADDOPTS` set a seed or disable it (`-p no:randomly`), the run draws a seed
and passes it as `--randomly-seed`, so an order-dependent failure can be replayed; a seed given
there is recorded as given. An ini file's `addopts` is not read: this repo has none.

Signals. pytest runs in its own session, so a terminal's CTRL+C or hangup reaches this script
alone. From before the start record until after the end record, SIGINT, SIGTERM and SIGHUP are
recorded, never fatal here (a signal this script inherited as ignored stays ignored). The first is
passed to pytest's process group as SIGINT (pytest stops and prints its summary). A later one, at
least 1 s after the one before it, or 30 s without an exit ($NEVR_TEST_INTERRUPT_GRACE_S), kills
the group; a repeat within 1 s (`just` passes on a SIGTERM the group also got) is recorded and not
escalated. A signal before pytest is started means it is never started; one that lands while it is
being started is passed on as soon as it has started. A signal after pytest exits is recorded and
does not change the outcome. When pytest has exited and something it left behind still holds its
output open for 10 s ($NEVR_TEST_DRAIN_S), pytest's process group is sent a kill
(`stray_kill_sent`), and `output_complete` says whether the output then closed. If this script is
killed outright, pytest is killed with it.

The echo. stdout, and stderr while pytest runs, are each written from a thread of their own, so a
stdout or stderr that nobody reads holds up only that echo, never the run, the log or the record.
`echo` in the end record is `complete`, `closed` (stdout went away), `stalled` (stdout stopped
taking lines; the echo was dropped, or did not finish within $NEVR_TEST_DRAIN_S in all) or `none`
(no stdout); `stderr_echo` says the same of stderr. When the echo to stdout is not complete, a
line on stderr names the log, which holds the whole output. A line stdout cannot encode is echoed
with replacements; the log keeps it as read.

Exit code: pytest's. Exceptions, each loud on stderr:
- 2 before pytest runs, when the log directory, the log or the start record cannot be written, the
  echo threads cannot start, `--junitxml` is passed (the run owns its XML path), or a wait above is
  not a positive number of seconds: a run whose record cannot be kept is refused, never run
  unrecorded;
- 2 when pytest cannot be started (`start_error`; the end record is still written);
- 128 + the number of the first signal when a signal interrupted the run (130 for SIGINT);
- 128 + n when pytest was killed by signal n that this script did not send (an OOM kill, say);
  `pytest_rc` keeps the negative code;
- when the log or the end record cannot be written, or pytest's output could not be read, pytest's
  exit code if it is non-zero, else 2: a run is never reported green without its record.

Usage: run_tool_tests.py [pytest args...]   (default: -q tools/tests)
Log directory: $NEVR_TEST_LOG_DIR, else /var/tmp/work-nevr-runtime/test-runs/tools-tests.
"""
import ctypes
import datetime
import getpass
import importlib.metadata
import importlib.util
import json
import math
import os
import pathlib
import queue
import random
import re
import secrets
import select
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


def seconds_from_env(name: str, default: float):
    """$name as a positive, finite number of seconds; `default` when unset; None when it is not one."""
    raw = os.environ.get(name)
    if not raw:
        return default
    try:
        value = float(raw)
    except ValueError:
        return None
    return value if math.isfinite(value) and value > 0 else None


# Seconds pytest gets after the first signal, and seconds its output may stay open after it exits, held
# by a process it left behind (also the most the echo is waited for).
WAITS = {"NEVR_TEST_INTERRUPT_GRACE_S": seconds_from_env("NEVR_TEST_INTERRUPT_GRACE_S", 30),
         "NEVR_TEST_DRAIN_S": seconds_from_env("NEVR_TEST_DRAIN_S", 10)}
INTERRUPT_GRACE_S = WAITS["NEVR_TEST_INTERRUPT_GRACE_S"]
DRAIN_S = WAITS["NEVR_TEST_DRAIN_S"]
# A repeat of a signal sooner than this after the one before it is the same event delivered twice.
REPEAT_S = 1.0
# Lines the echo may fall behind stdout before it is dropped.
ECHO_BACKLOG = 10000
HANDLED_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
PR_SET_PDEATHSIG = 1


# While a run is live, its stderr echo: a loud line then never blocks on a stderr nobody reads.
STDERR_ECHO = None


def report(text: str) -> None:
    """A loud line on stderr."""
    if STDERR_ECHO is not None:
        STDERR_ECHO.put(text + "\n")
    else:
        print(text, file=sys.stderr, flush=True)


def user():
    """The user running the script, or None when the system cannot say."""
    try:
        return getpass.getuser()
    except (OSError, KeyError):
        return None


def remove_log(path: pathlib.Path) -> None:
    """Remove the log of a refused run; say so when it cannot be removed."""
    try:
        path.unlink()
    except OSError as exc:
        print(f"test-tools: FAIL cannot remove the refused run's log {path}: {exc}", file=sys.stderr)


def signal_name(number: int) -> str:
    try:
        return signal.Signals(number).name
    except ValueError:
        return f"signal {number}"


def git(*args: str):
    """git's output, or None when git fails (not a repository, refused, not installed)."""
    try:
        out = subprocess.run(["git", *args], capture_output=True, text=True)
    except OSError:
        return None
    return out.stdout.strip() if out.returncode == 0 else None


def run_stem(start: datetime.datetime, head: str) -> str:
    """A file stem no other run shares, even one started in the same microsecond by the same process."""
    return f"{start.strftime('%Y%m%dT%H%M%S.%fZ')}-{os.getpid()}-{secrets.token_hex(8)}-{head[:12]}"


def failing_ids(xml_path: pathlib.Path) -> tuple:
    """(the id of every failed or errored test case, junit_state: ok, missing, unparseable or unreadable)."""
    if not xml_path.exists():
        return [], "missing"
    try:
        root = ET.parse(xml_path).getroot()
    except ET.ParseError:
        return [], "unparseable"
    except OSError:
        return [], "unreadable"
    ids = []
    for case in root.iter("testcase"):
        if case.find("failure") is not None or case.find("error") is not None:
            classname, name = case.get("classname", ""), case.get("name", "")
            ids.append(f"{classname}::{name}" if classname else name)
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


def randomly_seed(args: list, environ) -> tuple:
    """(extra args, seed) for pytest-randomly, from the arguments and the environment.

    No plugin, plugin autoload off without `-p randomly`, or `-p no:randomly`: nothing added, seed
    None. A given `--randomly-seed`: nothing added, that value recorded (an integer, or the word
    given, such as `last`). Otherwise a drawn seed is added.
    """
    if importlib.util.find_spec("pytest_randomly") is None:
        return [], None
    try:
        addopts = shlex.split(environ.get("PYTEST_ADDOPTS", ""))
    except ValueError:
        addopts = []  # pytest rejects it too, and says so in the log
    tokens = addopts + list(args)
    plugin_args = [tokens[i + 1] for i, tok in enumerate(tokens[:-1]) if tok == "-p"]
    plugin_args += [tok[2:] for tok in tokens if tok.startswith("-p") and len(tok) > 2]
    if "no:randomly" in plugin_args:
        return [], None
    if environ.get("PYTEST_DISABLE_PLUGIN_AUTOLOAD") and not {"randomly", "pytest_randomly"} & set(plugin_args):
        return [], None
    given = None
    for i, tok in enumerate(tokens):
        if tok.startswith("--randomly-seed="):
            given = tok.split("=", 1)[1]
        elif tok == "--randomly-seed" and i + 1 < len(tokens):
            given = tokens[i + 1]
    if given is not None:
        return [], int(given) if given.isascii() and given.isdigit() else given
    seed = random.SystemRandom().randrange(2**32)
    return [f"--randomly-seed={seed}"], seed


def append_record(index: pathlib.Path, record: dict) -> bool:
    try:
        with index.open("a+b") as f:
            f.seek(0, os.SEEK_END)
            lead = b""
            if f.tell() > 0:
                f.seek(-1, os.SEEK_END)
                if f.read(1) != b"\n":
                    lead = b"\n"  # a line an earlier write left cut short stays on its own line
            f.write(lead + (json.dumps(record) + "\n").encode("utf-8"))
        return True
    except OSError as exc:
        report(f"test-tools: FAIL cannot append the {record['event']} record to {index}: {exc}")
        return False


# Looked up here, not in the forked child, where another thread may hold a lock the lookup needs.
try:
    PRCTL = ctypes.CDLL(None, use_errno=True).prctl
except (OSError, AttributeError):
    PRCTL = None  # not Linux


def die_with_parent() -> None:
    """In pytest, before exec: a SIGKILL of this script kills pytest too (Linux; elsewhere a no-op)."""
    if PRCTL is not None:
        PRCTL(PR_SET_PDEATHSIG, signal.SIGKILL)


class Echo:
    """Lines to stdout or stderr from a thread of its own, straight to the file descriptor: a stream nobody
    reads blocks that thread alone, and holds no lock the rest of the process (or its exit) needs."""

    def __init__(self, stream):
        self.state = "complete"
        self.fd = None
        try:
            self.fd = stream.fileno()
        except (OSError, ValueError, AttributeError):  # closed, not a file, or None (no stdout)
            pass
        if self.fd is None:
            self.state = "none"
            return
        self.encoding = getattr(stream, "encoding", None) or "utf-8"
        self.lines = queue.Queue(maxsize=ECHO_BACKLOG)
        threading.Thread(target=self.write_lines, daemon=True).start()

    def put(self, text: str) -> None:
        if self.state != "complete":
            return
        try:
            self.lines.put_nowait(text)
        except queue.Full:
            self.state = "stalled"

    def write_lines(self) -> None:
        while True:
            item = self.lines.get()
            if isinstance(item, threading.Event):
                item.set()
                continue
            if self.state == "closed":
                continue
            data = item.encode(self.encoding, errors="replace")
            try:
                while data:
                    try:
                        data = data[os.write(self.fd, data):]
                    except BlockingIOError:  # a non-blocking stdout that is full: wait until it is not
                        select.select([], [self.fd], [], 1)
            except OSError:
                self.state = "closed"

    def flush(self, timeout: float) -> str:
        """Wait up to `timeout` in all for every line put so far to be written; the echo's state."""
        if self.state != "complete":
            return self.state
        deadline = time.monotonic() + timeout
        mark = threading.Event()
        try:
            self.lines.put(mark, timeout=timeout)
        except queue.Full:
            mark = None
        if (mark is None or not mark.wait(max(0.0, deadline - time.monotonic()))) and self.state == "complete":
            self.state = "stalled"
        return self.state


class Run:
    """The pytest process, its output and the signals that arrive while it runs."""

    def __init__(self, log):
        self.log = log
        self.log_closed = False
        self.lock = threading.Lock()
        self.echo = Echo(sys.stdout)
        self.errs = Echo(sys.stderr)
        self.log_error = None
        self.read_error = None
        self.summary = ""
        self.proc = None
        self.signals = []
        self.signals_at_exit = None  # how many had arrived when pytest exited
        self.start_error = None
        self.signal_times = []
        self.escalations = 0
        self.forwarded = 0
        self.stray_kill_sent = False

    def note_log_error(self, exc: Exception) -> None:
        if self.log_error is None:
            self.log_error = str(exc)
            report(f"test-tools: FAIL cannot write the log: {exc}")

    def emit(self, text: str) -> None:
        self.echo.put(text)
        with self.lock:
            if self.log_error is None and not self.log_closed:
                try:
                    self.log.write(text)
                    self.log.flush()
                except (OSError, ValueError) as exc:
                    self.note_log_error(exc)

    def close_log(self) -> None:
        """Close the log once; a line read after this (a process pytest left behind) is echoed only."""
        with self.lock:
            if self.log_closed:
                return
            self.log_closed = True
            try:
                self.log.close()
            except (OSError, ValueError) as exc:
                self.note_log_error(exc)

    def on_signal(self, signum, frame) -> None:
        now = time.monotonic()
        repeat = bool(self.signal_times) and now - self.signal_times[-1] < REPEAT_S
        self.signals.append(signal.Signals(signum).name)
        self.signal_times.append(now)
        if not repeat:
            self.escalations += 1
        self.forward()

    def forward(self) -> None:
        """Pass each escalation on to pytest's group: SIGINT first, SIGKILL after."""
        if self.proc is None or self.proc.poll() is not None:
            return
        while self.forwarded < self.escalations:
            first = self.forwarded == 0
            self.forwarded += 1
            self.kill_group(signal.SIGINT if first else signal.SIGKILL)

    def kill_group(self, sig) -> None:
        try:
            os.killpg(self.proc.pid, sig)
        except (ProcessLookupError, PermissionError):
            pass

    def pump(self) -> None:
        try:
            for line in self.proc.stdout:
                try:
                    self.emit(line)
                    if SUMMARY_RE.search(line):
                        self.summary = line.strip().strip("= ")
                except Exception as exc:  # noqa: BLE001 - any failure here must not stop the draining
                    if self.read_error is None:
                        self.read_error = f"{type(exc).__name__}: {exc}"
        except Exception as exc:  # noqa: BLE001 - the end record must say the output was cut short
            self.read_error = f"{type(exc).__name__}: {exc}"

    def wait(self) -> bool:
        """Wait for pytest and its output; True when the output was read to its end."""
        reader = threading.Thread(target=self.pump, daemon=True)
        reader.start()
        while True:
            try:
                self.proc.wait(timeout=0.2)
                self.signals_at_exit = len(self.signals)
                break
            except subprocess.TimeoutExpired:
                if self.signal_times and time.monotonic() - self.signal_times[0] > INTERRUPT_GRACE_S:
                    self.kill_group(signal.SIGKILL)
        reader.join(DRAIN_S)
        if reader.is_alive():
            # pytest has exited and something it started still holds the pipe.
            self.stray_kill_sent = True
            self.kill_group(signal.SIGKILL)
            reader.join(DRAIN_S)
        return not reader.is_alive() and self.read_error is None


def main(argv: list) -> int:
    if any(a == "--junitxml" or a.startswith("--junitxml=") or a.startswith("--junit-xml") for a in argv):
        print("test-tools: FAIL --junitxml is not accepted: the run writes its own XML beside its log",
              file=sys.stderr)
        return 2
    for name, value in WAITS.items():
        if value is None:
            print(f"test-tools: FAIL ${name}={os.environ.get(name)!r} is not a positive number of seconds",
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
    status = git("status", "--porcelain")
    tree = "unknown" if status is None else "DIRTY" if status else "clean"
    start = datetime.datetime.now(datetime.timezone.utc)
    stem = run_stem(start, head)
    log_path = log_dir / f"{stem}.log"
    xml_path = log_dir / f"{stem}.xml"
    index = log_dir / "runs.jsonl"
    args = argv or ["-q", "tools/tests"]
    seed_args, seed = randomly_seed(args, os.environ)
    cmd = [sys.executable, "-m", "pytest", f"--junitxml={xml_path}", *seed_args, *args]
    env = pytest_environment()
    base = {"start": start.isoformat(), "cwd": os.getcwd(), "user": user(), "head": head, "tree": tree,
            "cmd": cmd, "pytest": env["pytest"],
            "plugins": env["plugins"], "randomly_seed": seed, "log": str(log_path), "junit": str(xml_path)}

    try:
        log = log_path.open("w", encoding="utf-8")
    except OSError as exc:
        print(f"test-tools: FAIL cannot write the log {log_path}: {exc}", file=sys.stderr)
        return 2
    global STDERR_ECHO
    try:
        run = Run(log)
    except RuntimeError as exc:  # the echo threads could not start
        log.close()
        remove_log(log_path)
        print(f"test-tools: FAIL cannot start the run: {exc}", file=sys.stderr)
        return 2
    STDERR_ECHO = run.errs
    previous = {}
    for s in HANDLED_SIGNALS:
        if signal.getsignal(s) is not signal.SIG_IGN:
            previous[s] = signal.signal(s, run.on_signal)
    try:
        if not append_record(index, {"event": "start", **base}):
            remove_log(log_path)
            return 2
        for line in (f"test-tools: start {start.isoformat()}", f"test-tools: HEAD {head}",
                     f"test-tools: tree {tree}", f"test-tools: pytest {env['pytest']}",
                     f"test-tools: plugins {' '.join(f'{k}={v}' for k, v in env['plugins'].items())}",
                     f"test-tools: randomly-seed {seed}", f"test-tools: cmd {' '.join(cmd)}"):
            run.emit(line + "\n")
        output_complete = True
        if not run.signals:
            try:
                run.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                            encoding="utf-8", errors="replace", start_new_session=True,
                                            preexec_fn=die_with_parent)
            except (OSError, subprocess.SubprocessError) as exc:
                run.start_error = f"{type(exc).__name__}: {exc}"
                report(f"test-tools: FAIL pytest could not be started: {run.start_error}")
        if run.proc is not None:
            run.forward()
            output_complete = run.wait()
        pytest_rc = run.proc.returncode if run.proc else None
        # Only a signal that came before pytest exited (or kept it from starting) interrupted the run; a
        # later one is recorded in `signals` but does not change the outcome.
        before_exit = run.signals_at_exit if run.proc else len(run.signals)
        interrupted = before_exit > 0
        if interrupted:
            rc = 128 + signal.Signals[run.signals[0]].value
        elif run.start_error is not None:
            rc = 2
        elif pytest_rc is not None and pytest_rc < 0:  # a signal this script did not send
            rc = 128 - pytest_rc
            report(f"test-tools: FAIL pytest was killed by {signal_name(-pytest_rc)}")
        else:
            rc = pytest_rc
        end = datetime.datetime.now(datetime.timezone.utc)
        run.emit(f"test-tools: end {end.isoformat()} rc={rc}"
                 + (f" interrupted={run.signals[0]}" if interrupted else "")
                 + ("" if output_complete else " output=incomplete") + "\n")
        run.close_log()
        echo = run.echo.flush(DRAIN_S)
        stderr_echo = run.errs.flush(DRAIN_S)

        failed, junit_state = failing_ids(xml_path)
        record = {"event": "end", **base, "end": end.isoformat(), "rc": rc, "pytest_rc": pytest_rc,
                  "interrupted": interrupted, "signals": list(run.signals), "summary": run.summary,
                  "failed": failed, "junit_state": junit_state, "output_complete": output_complete,
                  "stray_kill_sent": run.stray_kill_sent, "echo": echo, "log_error": run.log_error,
                  "read_error": run.read_error, "start_error": run.start_error, "stderr_echo": stderr_echo}
        recorded = append_record(index, record)
        if echo != "complete":
            report(f"test-tools: the echo to stdout is {echo}; the whole output is in {log_path}")
        if run.read_error is not None:
            report(f"test-tools: FAIL pytest's output could not be read to its end: {run.read_error}")
        if not recorded or run.log_error is not None or run.read_error is not None:
            return rc if rc != 0 else 2
        run.echo.put(f"test-tools: log {log_path}\n")
        run.echo.flush(DRAIN_S)
        return rc
    finally:
        run.close_log()
        STDERR_ECHO = None
        run.errs.flush(DRAIN_S)
        for s, h in previous.items():
            signal.signal(s, h)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
