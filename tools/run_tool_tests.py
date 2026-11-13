#!/usr/bin/env python3
"""Run the tools/tests suite and keep the run (#913).

A run that is only printed is lost when the terminal scrolls: a one-off failure then has a count
and no name. Every run therefore writes, under the log directory:

- `<stem>.log`, the stem being `<UTC stamp to the microsecond>-<pid>-<16 random hex>-<sha12>`: a
  header (UTC start, HEAD, tree clean, DIRTY or unknown, pytest's version and the plugins installed
  on the machine (only those the command names with `-p` load), the pytest-randomly seed, the
  workers, the command), then pytest's full output, also echoed to stdout line by
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
  records name the checkout (`checkout`, git's top level; None outside a repository), the directory
  the run was started in (`cwd`; None when it no longer exists) and the user who ran it (`user`).

pytest's short summary (on by default, `-rfE`) names every failure and error even under `-q`; the
log keeps it whole. When pytest-randomly is installed (the run names it with `-p randomly`), and
neither the arguments nor `$PYTEST_ADDOPTS` set a seed or disable it (`-p no:randomly`), the run
draws a seed and passes it as `--randomly-seed`; a seed given there is recorded as given. On one
process the seed replays an order-dependent failure; on workers the order within each worker also
depends on which tests the worker was handed, which neither the XML nor the log records, so the
seed alone does not replay it (run with `-n 0` or `NEVR_TEST_WORKERS=0` to replay serially). An ini
file's `addopts` is not read: this repo has none.

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
(no stdout); `stderr_echo` says the same of stderr, up to the end record. When the echo to stdout is not complete, a
line on stderr names the log, which holds the whole output. A line stdout cannot encode is echoed
with replacements; the log keeps it as read.

The plugins. pytest is started with plugin autoload off (`PYTEST_DISABLE_PLUGIN_AUTOLOAD=1`: every pytest plugin
installed on the machine would otherwise load, about 5 s per pytest start) and with the two the suite uses named,
`-p xdist -p randomly`. With pytest-xdist the suite runs on `-n <workers>` processes. Unset, $NEVR_TEST_WORKERS gives
one worker per hardware thread and at most 8 (DEFAULT_WORKER_CEILING: the suite takes 302 s on 8 workers and 313 s on
16, and 1137 s on 1). Set, it is `logical` (one per hardware thread, no ceiling), `auto` (one per physical core) or a
whole number, 0 for serial. Without pytest-xdist the suite runs serially, with one `WARN` line on stderr. Both
records and the log's header carry `mode` (`parallel` or `serial`), `workers` and `serial_reason`, and `workers_source`
(`default`, `NEVR_TEST_WORKERS` or `-n in the arguments`); for the default, `workers_basis` names the CPUs, the ceiling
and which bound the count. A `-n` in the arguments replaces $NEVR_TEST_WORKERS and is what the records name (`-n 0` is
serial); `-p no:xdist` in the arguments makes the run serial.

The memory (#2195). The run reads the cgroup it runs in (`/proc/self/cgroup`; the root is $NEVR_TEST_CGROUP_ROOT, else
/sys/fs/cgroup), that cgroup alone: the kernel counts a kill in the cgroup of the process it killed and in every cgroup
above it, so a slice counts every scope's kills and says nothing about this run. The end record carries `memory`: the
cgroup's `memory.peak` read through a descriptor reset when the run starts (`peak_since` is `run start`; where the
kernel refuses the reset it is `cgroup creation`, the lifetime peak, which a fresh scope makes the run's), the cgroup's
`oom` and `oom_kill` events counted over the run (`memory.events.local`: kills of processes directly in that cgroup,
not in a cgroup below it, which a run's scope does not have; `memory.events` where a tree has no such file), and `oom_killed` with `oom_killed_in`: true when the run's own cgroup saw an oom_kill. The SIGTERM that systemd
sends the rest of a scope the kernel killed in is then that, not an interrupt. A `test-tools: memory ...` line precedes
the end line in the log, and `oom_killed=<cgroup>` follows the end line's `rc`.

The lock. A run of the whole suite is one at a time per log directory: an exclusive flock on <log dir>/run.lock,
taken before the start record and held until the run ends. A run is a subset, and takes no lock, when its positional
arguments name between one and SUBSET_MAX_FILES test paths and none is tools/tests or a directory above it (`is_subset`:
options and the values of the options that take one are not paths, so `--deselect X`, `--ignore X`, `--rootdir .`,
`tools`, `.` and a list of every test file all run the whole suite, and so does anything that hides what pytest will
collect: an unknown option, an `@file` argument, `-c`, `-o addopts=...`, a path in $PYTEST_ADDOPTS); its records say
`lock: {"taken": false, "reason": "subset"}`. The default log directory is the same for every checkout on a machine, so
the lock is machine-wide; a run with a log directory of its own has a lock of its own. The file names the holder (pid,
checkout, start) while it runs. A run that finds it held says who holds it on stderr and waits up to
$NEVR_TEST_LOCK_WAIT_S (default 5400); both records carry `lock` (`taken`, `state`, `contended`, `holder`, `wait_s`,
`deadline_s`; `taken` is true only for a run that holds the lock). Past the deadline the run does not start: exit 75 and
a `lock_timeout` record that names the holder then. A SIGINT, SIGTERM or SIGHUP during the wait stops it the same way:
exit 128 + the signal and a `lock_wait_interrupted` record.

The environment. pytest is started with a fixed one, so a run gives the same answer for whoever starts it:
`HERMETIC_SET` (no user or system git configuration, a fixed identity, no commit or tag signing, `main` as the
initial branch) and without the variables in `HERMETIC_UNSET` (no display for a test that starts Wine, no gpg home).
Both records, and the log's header, name what was set and which of the removed variables the caller had
(`env_set`, `env_removed`). The `verify` recipe's unittest line in the justfile carries the same variables, and a test
holds the two equal.

Exit code: pytest's. Exceptions, each loud on stderr:
- 75 when the lock was not free within $NEVR_TEST_LOCK_WAIT_S: no pytest ran;
- 128 + n when signal n came while the run waited for the lock: no pytest ran;
- 2 before pytest runs, when the log directory, the log or the start record cannot be written, the
  echo threads cannot start, `--junitxml` is passed (the run owns its XML path), a wait above is
  not a positive number of seconds, the lock file cannot be opened, or $NEVR_TEST_WORKERS is not `auto`, `logical` or a whole number: a run whose record cannot be kept is refused, never run
  unrecorded;
- 2 when pytest cannot be started (`start_error`; the end record is still written);
- 128 + the number of the first signal when a signal interrupted the run (130 for SIGINT);
- 128 + n when pytest was killed by signal n that this script did not send (an OOM kill, say);
  `pytest_rc` keeps the negative code;
- when the log or the end record cannot be written, or pytest's output could not be read, pytest's
  exit code if it is non-zero, else 2: a run is never reported green without its record.

Usage: run_tool_tests.py [pytest args...]   (default: -q tools/tests)
Log directory: $NEVR_TEST_LOG_DIR, else test-runs/tools-tests in the work root (tools/work_root.py:
$NEVR_WORK_ROOT, else src/nevr-runtime-work in the home directory).
"""
import ctypes
import datetime
import fcntl
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
import types
import xml.etree.ElementTree as ET

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import work_root  # noqa: E402

DEFAULT_LOG_DIR = str(work_root.work_root() / "test-runs" / "tools-tests")
LOG_DIR_PURPOSE = ("Records of the runs of the tools test suite (tools/run_tool_tests.py): per run its full "
                   "output, JUnit XML and the start and end records in runs.jsonl.")
LOG_DIR_REMOVED = "kept: the record of every run is the evidence a PR cites; remove a run only when no open PR cites it."
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


# The plugins the suite uses, as (name for `-p`, module that provides it); pytest starts with autoload off and these.
SUITE_PLUGINS = (("xdist", "xdist"), ("randomly", "pytest_randomly"))
# $NEVR_TEST_WORKERS when it is unset (a setting of that word is refused: only a count, `auto` or `logical` is one).
DEFAULT_WORKERS = "default"
# The worker count of a run that is given none: one per hardware thread, at most this many. Measured on the whole suite
# (#2195): 1 worker took 1137 s, 8 workers 302 s, 16 workers 313 s, so more than 8 buys no time; the run's cgroup peaked
# at 1.40 GiB with 1 worker, 3.20 GiB with 8 (2.73 GiB with a build beside it), 3.06 GiB with 16, and above 4.39 GiB with
# 32 (a 4500M cap killed that run). Eight is the fastest count and the one that fits a 6 GiB slice beside a build.
DEFAULT_WORKER_CEILING = 8
CGROUP_ROOT = "/sys/fs/cgroup"
LOCK_NAME = "run.lock"
EXIT_LOCK_TIMEOUT = 75

# Seconds pytest gets after the first signal, and seconds its output may stay open after it exits, held
# by a process it left behind (also the most the echo is waited for); and seconds a run waits for the lock.
WAITS = {"NEVR_TEST_INTERRUPT_GRACE_S": seconds_from_env("NEVR_TEST_INTERRUPT_GRACE_S", 30),
         "NEVR_TEST_DRAIN_S": seconds_from_env("NEVR_TEST_DRAIN_S", 10),
         "NEVR_TEST_LOCK_WAIT_S": seconds_from_env("NEVR_TEST_LOCK_WAIT_S", 5400)}
INTERRUPT_GRACE_S = WAITS["NEVR_TEST_INTERRUPT_GRACE_S"]
DRAIN_S = WAITS["NEVR_TEST_DRAIN_S"]
LOCK_WAIT_S = WAITS["NEVR_TEST_LOCK_WAIT_S"]
# A repeat of a signal sooner than this after the one before it is the same event delivered twice.
REPEAT_S = 1.0
# What every child pytest is started with: git reads no user or system configuration and finds a fixed identity, never
# signs, and starts a repository on `main`; and none of HERMETIC_UNSET (a display would let a test that starts Wine
# reach the user's desktop; a gpg home would let a signing attempt find the user's key).
HERMETIC_GIT_CONFIG = (("user.name", "nevr-test"), ("user.email", "test@example.invalid"),
                       ("commit.gpgsign", "false"), ("tag.gpgsign", "false"), ("init.defaultBranch", "main"))
HERMETIC_SET = {"GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_CONFIG_COUNT": str(len(HERMETIC_GIT_CONFIG)),
                **{f"GIT_CONFIG_{kind}_{i}": text for i, pair in enumerate(HERMETIC_GIT_CONFIG)
                   for kind, text in zip(("KEY", "VALUE"), pair)}}
# The caller's git authorship and config-parameter variables go too: git sets GIT_CONFIG_PARAMETERS for `git -c` and
# for aliases, and the authorship ones would name another author in a commit the tests make (#2149).
HERMETIC_UNSET = ("DISPLAY", "WAYLAND_DISPLAY", "GNUPGHOME", "GIT_CONFIG_PARAMETERS", "GIT_AUTHOR_NAME",
                  "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL")
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


def cwd():
    """The directory the run was started in, or None when it no longer exists."""
    try:
        return os.getcwd()
    except OSError:
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


def hermetic_environment(environ) -> tuple:
    """(the environment pytest is started with, the variables of HERMETIC_UNSET `environ` had).

    A GIT_CONFIG_KEY_n or GIT_CONFIG_VALUE_n of the caller's past HERMETIC_SET's count is left where it is: git reads
    only the first GIT_CONFIG_COUNT of them."""
    removed = [name for name in HERMETIC_UNSET if name in environ]
    env = {k: v for k, v in environ.items() if k not in HERMETIC_UNSET}
    env.update(HERMETIC_SET)
    return env, removed


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


def option_tokens(args: list, environ) -> list:
    """$PYTEST_ADDOPTS (split as pytest splits it) followed by the arguments."""
    try:
        addopts = shlex.split(environ.get("PYTEST_ADDOPTS", ""))
    except ValueError:
        addopts = []  # pytest rejects it too, and says so in the log
    return addopts + list(args)


def plugin_names(tokens: list) -> list:
    """The names given with `-p NAME` or `-pNAME`."""
    names = [tokens[i + 1] for i, tok in enumerate(tokens[:-1]) if tok == "-p"]
    return names + [tok[2:] for tok in tokens if tok.startswith("-p") and len(tok) > 2]


def workers_setting(environ):
    """$NEVR_TEST_WORKERS as `auto`, `logical` or a whole number, `default` when it is unset or empty; None when it is
    none of them."""
    raw = environ.get("NEVR_TEST_WORKERS")
    if not raw:
        return DEFAULT_WORKERS
    if raw in ("auto", "logical"):
        return raw
    return int(raw) if raw.isascii() and raw.isdigit() else None


def given_workers(args: list):
    """The worker count the arguments set, as the last `-n N`, `-nN`, `--numprocesses N` or `--numprocesses=N`
    names it (argparse keeps the last): None when they set none, else the word as given."""
    given = None
    for i, tok in enumerate(args):
        if tok in ("-n", "--numprocesses") and i + 1 < len(args):
            given = args[i + 1]
        elif tok.startswith("--numprocesses="):
            given = tok.split("=", 1)[1]
        elif tok.startswith("-n") and len(tok) > 2 and not tok.startswith("--"):
            given = tok[2:]
    return given


SUITE_DIR = pathlib.Path(__file__).resolve().parent / "tests"
# Every option of the pytest this suite runs under (with `-p xdist -p randomly`, as the runner starts it): those that
# take a value (`--opt value`; `--opt=value` and `-xVALUE` are one argument; `--debug` and `--cache-show` take an optional
# one, which the classifier reads as given) and those that do not. `tools/tests/test_run_tool_tests_memory.py` compares
# both with pytest's own parser, so a new pytest option fails a test and does not pass for a test path.
VALUE_OPTIONS = frozenset((
    "--assert", "--basetemp", "--cache-show", "--capture", "--code-highlight", "--color", "--confcutdir",
    "--config-file", "--debug", "--deselect", "--dist", "--doctest-glob", "--doctest-report", "--durations",
    "--durations-min", "--ignore", "--ignore-glob", "--import-mode", "--junit-prefix", "--junit-xml", "--junitprefix",
    "--junitxml", "--last-failed-no-failures", "--lfnf", "--log-auto-indent", "--log-cli-date-format",
    "--log-cli-format", "--log-cli-level", "--log-date-format", "--log-disable", "--log-file",
    "--log-file-date-format", "--log-file-format", "--log-file-level", "--log-file-mode", "--log-format",
    "--log-level", "--max-warnings", "--max-worker-restart", "--maxfail", "--maxprocesses", "--maxschedchunk",
    "--numprocesses", "--override-ini", "--pastebin", "--pdbcls", "--px", "--pythonwarnings", "--randomly-seed",
    "--report-chars", "--rootdir", "--rsyncdir", "--rsyncignore", "--show-capture", "--tb", "--testrunuid", "--tx",
    "--verbosity", "-W", "-c", "-k", "-m", "-n", "-o", "-p", "-r"))
FLAG_OPTIONS = frozenset((
    "--cache-clear", "--co", "--collect-in-virtualenv", "--collect-only", "--collectonly",
    "--continue-on-collection-errors", "--disable-plugin-autoload", "--disable-pytest-warnings", "--disable-warnings",
    "--doctest-continue-on-failure", "--doctest-ignore-import-errors", "--doctest-modules", "--exitfirst",
    "--failed-first", "--ff", "--fixtures", "--fixtures-per-test", "--force-short-summary", "--full-trace",
    "--fulltrace", "--funcargs", "--help", "--keep-duplicates", "--keepduplicates", "--last-failed", "--lf",
    "--loadscope-reorder", "--markers", "--new-first", "--nf", "--no-fold-skipped", "--no-header",
    "--no-loadscope-reorder", "--no-showlocals", "--no-summary", "--noconftest", "--pdb", "--pyargs", "--quiet",
    "--randomly-dont-reorganize", "--randomly-dont-reset-seed", "--runxfail", "--setup-only", "--setup-plan",
    "--setup-show", "--setuponly", "--setupplan", "--setupshow", "--showlocals", "--stepwise", "--stepwise-reset",
    "--stepwise-skip", "--strict", "--strict-config", "--strict-markers", "--sw", "--sw-reset", "--sw-skip",
    "--trace", "--trace-config", "--traceconfig", "--verbose", "--version", "--xfail-tb", "-V", "-d", "-h", "-l",
    "-q", "-s", "-v", "-x"))
# More test files than this named on the command line are the suite file by file.
SUBSET_MAX_FILES = 20


# The options that bring in a config file, whose `addopts` can name any path (`-c`), and that set `addopts` itself
# (`-o addopts=...`).
CONFIG_OPTIONS = frozenset(("-c", "--config-file"))
OVERRIDE_INI_OPTIONS = frozenset(("-o", "--override-ini"))


def is_subset(args: list, environ) -> bool:
    """True when the arguments name a few test paths and so run a small part of the suite; such a run takes no lock.

    The tokens are $PYTEST_ADDOPTS (split as pytest splits it) followed by the arguments, as pytest reads them. Only
    the positional arguments name what is collected: the options are skipped, and so is the value of an option that
    takes one (the next argument unless it is an option). A positional argument names a path when what precedes any
    `::` exists. A path outside the repository, as a temporary suite is, makes a subset.

    The run is the whole suite when no positional argument names a path (the default run, or only options such as
    `--deselect` and `--ignore`, which collect from the root), when one is `tools/tests` or a directory above it
    (`tools`, `.`), or when more than SUBSET_MAX_FILES are named (the suite file by file). It is also the whole suite
    whenever the classifier cannot see what pytest will collect: an option that is not one pytest is known to have
    (VALUE_OPTIONS, FLAG_OPTIONS), whose value could be a path; any token that starts with `@` (argparse reads the file
    it names as more arguments, wherever the token stands); a config file (`-c`, `--config-file`) or an `addopts`
    override (`-o addopts=...`, `-o=addopts=...`), either of which can name any path; a positional argument in
    $PYTEST_ADDOPTS; and a $PYTEST_ADDOPTS that cannot be split. Anything in doubt locks."""
    try:
        addopts = shlex.split(environ.get("PYTEST_ADDOPTS", ""))
    except ValueError:
        return False  # pytest refuses it; what the run would collect is not known
    tokens = addopts + list(args)
    paths = []
    skip = None  # the option whose value is the next token
    options_end = False
    for index, tok in enumerate(tokens):
        if tok.startswith("@"):
            return False
        if skip:
            option, skip = skip, None
            if not tok.startswith("-"):
                if option in OVERRIDE_INI_OPTIONS and tok.startswith("addopts"):
                    return False
                continue
        if options_end or not tok.startswith("-"):
            if index < len(addopts):
                return False
            target = tok.split("::", 1)[0]
            if target and os.path.exists(target):
                paths.append(pathlib.Path(target).resolve())
        elif tok == "--":
            options_end = True
        elif tok.startswith("--"):
            name, attached, value = tok.partition("=")
            if name in CONFIG_OPTIONS:
                return False
            if name in VALUE_OPTIONS:
                if name in OVERRIDE_INI_OPTIONS and value.startswith("addopts"):
                    return False
                skip = None if attached else name
            elif name not in FLAG_OPTIONS:
                return False
        else:  # -x, -xyz (flags, the last of which may take the next argument), -kEXPR (a value attached)
            for i, letter in enumerate(tok[1:]):
                option = "-" + letter
                if option in CONFIG_OPTIONS:
                    return False
                if option in VALUE_OPTIONS:
                    value = tok[i + 2:]
                    # argparse drops the `=` of `-o=VALUE` and `-qo=VALUE`
                    if option in OVERRIDE_INI_OPTIONS and value.removeprefix("=").startswith("addopts"):
                        return False
                    skip = None if value else option
                    break
                if option not in FLAG_OPTIONS:
                    return False
    if not paths or len(paths) > SUBSET_MAX_FILES:
        return False
    return not any(p == SUITE_DIR or p in SUITE_DIR.parents for p in paths)


def logical_cpus() -> int:
    """The hardware threads xdist's `logical` counts (it honours $PYTEST_XDIST_AUTO_NUM_WORKERS); os.cpu_count()
    when xdist's own count is not reachable."""
    try:
        from xdist.plugin import pytest_xdist_auto_num_workers
        return int(pytest_xdist_auto_num_workers(types.SimpleNamespace(option=types.SimpleNamespace(
            numprocesses="logical"))))
    except (ImportError, AttributeError, TypeError, ValueError):
        return os.cpu_count() or 1


def own_cgroup(text: str):
    """The cgroup (version 2) a process runs in, from the text of its /proc/<pid>/cgroup: the path of the `0::` line;
    None when there is none (a system with only version 1 lines)."""
    for line in text.splitlines():
        if line.startswith("0::"):
            return line[3:] or "/"
    return None


def read_text(path: pathlib.Path):
    """The file's text, or None when it cannot be read."""
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return None


def read_value(path: pathlib.Path):
    """A cgroup file holding one whole number; None when it is absent, unreadable, or not a number (`max`)."""
    text = read_text(path)
    if text is None:
        return None
    text = text.strip()
    return int(text) if text.isascii() and text.isdigit() else None


def read_keys(path: pathlib.Path):
    """A flat-keyed cgroup file (`memory.events`, `memory.stat`: one `name number` per line) as a dict; None when it
    cannot be read."""
    text = read_text(path)
    if text is None:
        return None
    keys = {}
    for line in text.splitlines():
        name, _, number = line.partition(" ")
        if number.strip().isascii() and number.strip().isdigit():
            keys[name] = int(number)
    return keys


def cgroup_dir(root, cgroup: str) -> pathlib.Path:
    return pathlib.Path(root) / cgroup.lstrip("/")


class MemoryWatch:
    """What the run's own cgroup says about memory: its peak, and the kernel's OOM kills of processes in it.

    Only the run's own cgroup is read. The kernel counts a kill in the cgroup of the process it killed and in every
    cgroup above it, so a slice's `memory.events` counts every scope's kills, and a kill in another scope says nothing
    about this run. The kills are `memory.events.local` (those of processes directly in this cgroup; `memory.events`
    where a tree has no such file), counted from the start: a shared cgroup keeps the counts of every run before this
    one. The peak is read through a descriptor opened, and reset by a write, at the start (kernel 6.12 and later: the
    reset is per descriptor), so it is the run's peak and not the cgroup's lifetime peak; where the reset is refused
    (`peak_since` says `cgroup creation`), it is the lifetime peak, which a fresh scope makes the run's."""

    def __init__(self, root, cgroup):
        self.cgroup = cgroup
        self.directory = cgroup_dir(root, cgroup) if cgroup is not None else None
        self.events_at_start = self.events()
        self.peak_fd, self.peak_reset = self.open_peak()

    def events(self):
        if self.directory is None:
            return None
        local = read_keys(self.directory / "memory.events.local")
        return local if local is not None else read_keys(self.directory / "memory.events")

    def open_peak(self) -> tuple:
        """(a descriptor on memory.peak or None, whether its peak was reset to this moment)."""
        if self.directory is None:
            return None, False
        path = self.directory / "memory.peak"
        try:
            fd = os.open(path, os.O_RDWR)
        except OSError:
            try:
                return os.open(path, os.O_RDONLY), False
            except OSError:
                return None, False
        try:
            os.write(fd, b"0")
            return fd, True
        except OSError:
            return fd, False

    def peak(self):
        if self.peak_fd is None:
            return None
        try:
            text = os.pread(self.peak_fd, 64, 0).decode("ascii", errors="replace").strip()
        except OSError:
            return None
        return int(text) if text.isascii() and text.isdigit() else None

    @staticmethod
    def delta(before, after, name: str) -> int:
        return max(0, after.get(name, 0) - before.get(name, 0)) if before is not None and after is not None else 0

    def report(self) -> dict:
        """The end record's `memory`; closes the peak descriptor."""
        now = self.events()
        peak = self.peak()
        if self.peak_fd is not None:
            os.close(self.peak_fd)
            self.peak_fd = None
        if self.cgroup is None:
            state = "no cgroup v2"
        elif now is None and peak is None:
            state = "unreadable"
        else:
            state = "ok"
        kills = self.delta(self.events_at_start, now, "oom_kill")
        return {"state": state, "cgroup": self.cgroup, "peak_bytes": peak,
                "peak_since": "run start" if self.peak_reset else "cgroup creation",
                "oom": self.delta(self.events_at_start, now, "oom"), "oom_kill": kills,
                "oom_killed": kills > 0, "oom_killed_in": self.cgroup if kills > 0 else None}


class LockInterrupted(Exception):
    """A signal arrived while the run waited for the lock."""

    def __init__(self, signum: int):
        super().__init__(signum)
        self.signum = signum


def lock_holder(path: pathlib.Path):
    """Who the lock file says holds the lock: its JSON ({"pid", "checkout", "start"}), else its text, else None."""
    text = (read_text(path) or "").strip()
    if not text:
        return None
    try:
        return json.loads(text)
    except ValueError:
        return text


def held_by_parent(holder) -> bool:
    """True when the lock file names this process's parent, a test-go-tools run, as its holder. The caller has just found
    the lock held, so the file is not left over from a run that ended."""
    return (isinstance(holder, dict) and holder.get("tool") == "test-go-tools" and isinstance(holder.get("pid"), int)
            and holder["pid"] == os.getppid())


class MachineLock:
    """One run at a time per log directory: an exclusive `flock` on <log dir>/run.lock, held from before the start
    record until the run ends (or the process does: a killed run frees it). Every seat on a machine shares the default
    log directory, so the lock is machine-wide; a test that keeps its records in a directory of its own has a lock of
    its own, so a run a test starts never waits for the run that started the test. The file holds the holder's pid and
    checkout while it runs, for the line a waiting run prints and the record of one that gives up."""

    def __init__(self):
        self.fd = None
        self.info = None

    def acquire(self, log_dir: pathlib.Path, deadline_s: float) -> dict:
        """Take the lock, waiting up to `deadline_s`. Returns {"taken" (this run holds the lock), "path", "state",
        "contended" (another run held it when this one asked), "holder" (who held it then), "wait_s", "deadline_s",
        "signal"}; `state` is `acquired`, `timeout` or `interrupted` (a SIGINT, SIGTERM or SIGHUP came while it waited,
        named by `signal`). A lock file that cannot be opened raises OSError."""
        path = log_dir / LOCK_NAME
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)  # close-on-exec: pytest does not inherit it
        started = time.monotonic()
        info = {"taken": False, "path": str(path), "state": "acquired", "contended": False, "holder": None,
                "wait_s": 0.0, "deadline_s": deadline_s, "signal": None}
        previous = self.catch_signals()
        keep = False
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                keep = True
            except BlockingIOError:
                info["holder"] = lock_holder(path)
                if held_by_parent(info["holder"]):
                    # test-go-tools holds the lock across both halves of `just test-tools` (ADR 0044) and started this
                    # run: the lock is this run's already, and asking for it again would wait on the process that waits for us.
                    info.update(state="inherited", reason=f"held by test-go-tools (pid {os.getppid()}) across both halves")
                    os.close(fd)
                    self.info = info
                    return info
                info["contended"] = True
                report(f"test-tools: waiting for the machine-wide test lock {path} (holder: "
                       f"{json.dumps(info['holder'])}); giving up after {deadline_s:g} s")
                if self.block(fd, deadline_s):
                    keep = True
                else:
                    info["state"], info["holder"] = "timeout", lock_holder(path)
        except LockInterrupted as exc:
            info["state"], info["signal"], info["holder"] = "interrupted", signal_name(exc.signum), lock_holder(path)
            keep = False
        finally:
            for s, handler in previous.items():
                signal.signal(s, handler)
        info["wait_s"] = round(time.monotonic() - started, 6)
        if not keep:
            os.close(fd)
            self.info = info
            return info
        info["taken"] = True
        os.ftruncate(fd, 0)
        os.pwrite(fd, json.dumps({"pid": os.getpid(), "checkout": git("rev-parse", "--show-toplevel"),
                                  "start": datetime.datetime.now(datetime.timezone.utc).isoformat()}).encode(), 0)
        self.fd, self.info = fd, info
        return info

    @staticmethod
    def catch_signals() -> dict:
        """Make SIGINT, SIGTERM and SIGHUP raise LockInterrupted until the caller puts back what this returns (a signal
        this process inherited as ignored stays ignored)."""
        def interrupted(signum, frame):
            raise LockInterrupted(signum)

        return {s: signal.signal(s, interrupted) for s in HANDLED_SIGNALS if signal.getsignal(s) is not signal.SIG_IGN}

    @staticmethod
    def block(fd: int, deadline_s: float) -> bool:
        """Wait for the lock on `fd`; False when `deadline_s` seconds pass first (an interval timer interrupts the
        wait: no polling)."""
        def expired(signum, frame):
            raise TimeoutError

        previous = signal.signal(signal.SIGALRM, expired)
        signal.setitimer(signal.ITIMER_REAL, deadline_s)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            return True
        except TimeoutError:
            return False
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, previous)

    def release(self) -> None:
        if self.fd is not None:
            try:
                os.ftruncate(self.fd, 0)  # the file names no holder once the run ends
            except OSError:
                pass
            os.close(self.fd)  # closing the descriptor releases the lock
            self.fd = None


def plugin_plan(args: list, environ, workers) -> dict:
    """What the plugins and the workers add to pytest's command: {"flags", "mode", "workers", "serial_reason"}.

    `-p NAME` for each suite plugin that is installed and not switched off (`-p no:NAME`) in the arguments or
    $PYTEST_ADDOPTS, then `-n <workers>` when pytest-xdist is loaded and `workers` (`auto`, `logical` or a number)
    is not 0. A `-n` in the arguments replaces `workers` and is not added a second time; the record names the count
    it gives (`-n 0` is a serial run). A count of `auto` or `logical` is counted here, by xdist's own rule, so the
    record names the number pytest is given.
    """
    named = set(plugin_names(option_tokens(args, environ)))
    flags = []
    loaded = set()
    for name, module in SUITE_PLUGINS:
        if f"no:{name}" not in named and importlib.util.find_spec(module) is not None:
            flags += ["-p", name]
            loaded.add(name)
    given = given_workers(args)
    if given is not None:
        source = "-n in the arguments"
        workers = given if given in ("auto", "logical") else int(given) if given.isascii() and given.isdigit() else None
    else:
        source = "NEVR_TEST_WORKERS"
    if workers == 0:
        reason = f"{source} is 0" if given is not None else "NEVR_TEST_WORKERS=0"
    elif "xdist" in loaded and workers is not None:
        if not isinstance(workers, int):
            try:
                from xdist.plugin import pytest_xdist_auto_num_workers
                workers = pytest_xdist_auto_num_workers(types.SimpleNamespace(option=types.SimpleNamespace(
                    numprocesses=workers)))
            except (ImportError, AttributeError, TypeError) as exc:
                # xdist's own count is not reachable: hand pytest the word and record it as given.
                return {"flags": flags if given is not None else [*flags, "-n", workers], "mode": "parallel",
                        "workers": workers, "serial_reason": f"worker count left to xdist ({type(exc).__name__})"}
        if workers > 0:
            return {"flags": flags if given is not None else [*flags, "-n", str(workers)], "mode": "parallel",
                    "workers": workers, "serial_reason": None}
        reason = f"xdist counted {workers} workers"
    elif "no:xdist" in named:
        reason = "-p no:xdist"
    elif "xdist" not in loaded:
        reason = "pytest-xdist is not installed"
    else:
        reason = f"{source} is not a count"
    return {"flags": flags, "mode": "serial", "workers": 0, "serial_reason": reason}


def randomly_seed(args: list, environ) -> tuple:
    """(extra args, seed) for pytest-randomly, from the arguments and the environment.

    No plugin, plugin autoload off without `-p randomly`, or `-p no:randomly`: nothing added, seed
    None. A given `--randomly-seed`: nothing added, that value recorded (an integer, or the word
    given, such as `last`). Otherwise a drawn seed is added.
    """
    if importlib.util.find_spec("pytest_randomly") is None:
        return [], None
    tokens = option_tokens(args, environ)
    plugin_args = plugin_names(tokens)
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
    lock = MachineLock()
    try:
        return run_locked(argv, lock)
    finally:
        lock.release()


def run_locked(argv: list, lock: MachineLock) -> int:
    if any(a == "--junitxml" or a.startswith("--junitxml=") or a.startswith("--junit-xml") for a in argv):
        print("test-tools: FAIL --junitxml is not accepted: the run writes its own XML beside its log",
              file=sys.stderr)
        return 2
    for name, value in WAITS.items():
        if value is None:
            print(f"test-tools: FAIL ${name}={os.environ.get(name)!r} is not a positive number of seconds",
                  file=sys.stderr)
            return 2
    workers = workers_setting(os.environ)
    if workers is None:
        print(f"test-tools: FAIL $NEVR_TEST_WORKERS={os.environ.get('NEVR_TEST_WORKERS')!r} is not auto, logical or "
              "a whole number", file=sys.stderr)
        return 2

    log_dir = pathlib.Path(os.environ.get("NEVR_TEST_LOG_DIR") or DEFAULT_LOG_DIR)
    try:
        work_root.ensure_dir(log_dir, LOG_DIR_PURPOSE, LOG_DIR_REMOVED)
        with tempfile.NamedTemporaryFile(dir=log_dir, prefix=".writable-"):
            pass
    except OSError as exc:
        print(f"test-tools: FAIL cannot write the run record to {log_dir}: {exc}", file=sys.stderr)
        return 2

    if is_subset(argv, os.environ):
        lock_info = {"taken": False, "reason": "subset"}
    else:
        try:
            lock_info = lock.acquire(log_dir, LOCK_WAIT_S)
        except OSError as exc:
            print(f"test-tools: FAIL cannot take the machine-wide test lock in {log_dir}: {exc}", file=sys.stderr)
            return 2
    if lock_info.get("state") in ("timeout", "interrupted"):
        stopped = (f"the machine-wide test lock {lock_info['path']} was not free after {lock_info['wait_s']:g} s (the "
                   f"deadline is $NEVR_TEST_LOCK_WAIT_S, {LOCK_WAIT_S:g} s)" if lock_info["state"] == "timeout" else
                   f"{lock_info['signal']} stopped this run after {lock_info['wait_s']:g} s of waiting for the "
                   f"machine-wide test lock {lock_info['path']}")
        print(f"test-tools: FAIL {stopped}; it is held by {json.dumps(lock_info['holder'])}, so this run did not start",
              file=sys.stderr)
        append_record(log_dir / "runs.jsonl", {
            "event": "lock_timeout" if lock_info["state"] == "timeout" else "lock_wait_interrupted",
            "time": datetime.datetime.now(datetime.timezone.utc).isoformat(), "checkout": git("rev-parse", "--show-toplevel"),
            "cwd": cwd(), "user": user(), "lock": lock_info})
        return EXIT_LOCK_TIMEOUT if lock_info["state"] == "timeout" else 128 + signal.Signals[lock_info["signal"]].value
    head = git("rev-parse", "HEAD") or "unknown"
    status = git("status", "--porcelain")
    tree = "unknown" if status is None else "DIRTY" if status else "clean"
    start = datetime.datetime.now(datetime.timezone.utc)
    stem = run_stem(start, head)
    log_path = log_dir / f"{stem}.log"
    xml_path = log_dir / f"{stem}.xml"
    index = log_dir / "runs.jsonl"
    args = argv or ["-q", "tools/tests"]
    cgroup_root = os.environ.get("NEVR_TEST_CGROUP_ROOT") or CGROUP_ROOT
    watch = MemoryWatch(cgroup_root, own_cgroup(read_text(pathlib.Path("/proc/self/cgroup")) or ""))
    workers_basis = None
    if given_workers(args) is not None:
        workers_source = "-n in the arguments"
    elif workers != DEFAULT_WORKERS:
        workers_source = "NEVR_TEST_WORKERS"
    else:
        workers_source = "default"
        cpus = logical_cpus()
        workers = min(cpus, DEFAULT_WORKER_CEILING)
        workers_basis = {"cpus": cpus, "ceiling": DEFAULT_WORKER_CEILING, "workers": workers,
                         "limited_by": "ceiling" if cpus > DEFAULT_WORKER_CEILING else "cpu"}
    # The plugins and workers come before the arguments, so an argument (`-n 2`, `-p no:xdist`) wins.
    plan = plugin_plan(args, os.environ, workers)
    child_env, env_removed = hermetic_environment(os.environ)
    child_env["PYTEST_DISABLE_PLUGIN_AUTOLOAD"] = "1"
    seed_args, seed = randomly_seed([*plan["flags"], *args], child_env)
    cmd = [sys.executable, "-m", "pytest", f"--junitxml={xml_path}", *plan["flags"], *seed_args, *args]
    env = pytest_environment()
    base = {"start": start.isoformat(), "checkout": git("rev-parse", "--show-toplevel"), "cwd": cwd(),
            "user": user(), "head": head, "tree": tree,
            "cmd": cmd, "pytest": env["pytest"],
            "plugins": env["plugins"], "plugin_autoload": "off", "mode": plan["mode"], "workers": plan["workers"],
            "serial_reason": plan["serial_reason"], "workers_source": workers_source,
            "workers_basis": workers_basis, "lock": lock_info, "randomly_seed": seed, "env_set": HERMETIC_SET,
            "env_removed": env_removed, "log": str(log_path), "junit": str(xml_path)}

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
    if plan["serial_reason"] == "pytest-xdist is not installed":
        report("test-tools: WARN pytest-xdist is not installed: the suite runs serially, about ten times slower")
    if str(plan["serial_reason"]).startswith("worker count left to xdist"):
        report(f"test-tools: WARN {plan['serial_reason']}: pytest is given `-n {plan['workers']}` and counts the "
               "workers itself; the records name the word, not a number")
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
                     f"test-tools: installed plugins {' '.join(f'{k}={v}' for k, v in env['plugins'].items())}",
                     f"test-tools: randomly-seed {seed}",
                     f"test-tools: workers {plan['workers']} ({plan['mode']}"
                     + (f": {plan['serial_reason']}" if plan["serial_reason"] else "") + ")",
                     f"test-tools: worker basis source={workers_source} "
                     + (json.dumps(workers_basis, sort_keys=True) if workers_basis else "given"),
                     (f"test-tools: lock {lock_info['path']} {lock_info['state']} after {lock_info['wait_s']:g} s "
                      f"(deadline {lock_info['deadline_s']:g} s)" if lock_info["taken"] else
                      f"test-tools: lock {lock_info['path']} not taken here ({lock_info['reason']})"
                      if lock_info.get("state") == "inherited" else
                      f"test-tools: lock not taken ({lock_info['reason']}: the arguments name test paths)"),
                     f"test-tools: env set {' '.join(f'{k}={v}' for k, v in HERMETIC_SET.items())}",
                     f"test-tools: env removed {' '.join(env_removed) or 'none'}", f"test-tools: cmd {' '.join(cmd)}"):
            run.emit(line + "\n")
        output_complete = True
        if not run.signals:
            try:
                run.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                            env=child_env, encoding="utf-8", errors="replace", start_new_session=True,
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
        memory = watch.report()
        run.emit(f"test-tools: memory peak={memory['peak_bytes']} since={memory['peak_since']!r} "
                 f"cgroup={memory['cgroup']} state={memory['state']} oom={memory['oom']} "
                 f"oom_kill={memory['oom_kill']}\n")
        if memory["oom_killed"]:
            report(f"test-tools: FAIL the kernel's OOM killer stopped this run: {memory['oom_killed_in']} "
                   f"saw an oom_kill while it ran (peak {memory['peak_bytes']} bytes); the signal in the record "
                   "is its consequence, not an interrupt")
        run.emit(f"test-tools: end {end.isoformat()} rc={rc}"
                 + (f" interrupted={run.signals[0]}" if interrupted else "")
                 + (f" oom_killed={memory['oom_killed_in']}" if memory["oom_killed"] else "")
                 + ("" if output_complete else " output=incomplete") + "\n")
        run.close_log()
        echo = run.echo.flush(DRAIN_S)
        stderr_echo = run.errs.flush(DRAIN_S)

        failed, junit_state = failing_ids(xml_path)
        record = {"event": "end", **base, "end": end.isoformat(), "rc": rc, "pytest_rc": pytest_rc,
                  "interrupted": interrupted, "signals": list(run.signals), "summary": run.summary,
                  "failed": failed, "junit_state": junit_state, "output_complete": output_complete,
                  "memory": memory, "oom_killed": memory["oom_killed"], "oom_killed_in": memory["oom_killed_in"],
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
