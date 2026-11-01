#!/usr/bin/env python3
"""Gate for the heavy recipes: refuse them from a Claude Code session without the opt-in, and run one at a time.

The justfile's `_heavy-guard` recipe calls this as the first dependency of every heavy recipe:

    heavy_guard.py <recipe> [recipe arguments...]    the guard; exit 0 to run, 2 to refuse, 1 when it cannot decide
    heavy_guard.py strip-optin [arguments...]        the arguments without the opt-in, NUL-separated (for recipes that forward them)

Decisions, in order:
  1. nested: a process above this one is the holder of the heavy lock, the run that was started with the opt-in. The
     recipe is one of that run's own steps (`verify` calls `just build`), so it runs without taking the lock again.
     The check reads the process ancestry (/proc), not the environment: an environment variable can be set by hand.
  2. refused: CLAUDECODE is set (not empty, not "0") and the opt-in is absent. The refusal names the recipe only.
  3. acquired: otherwise the guard takes the host-wide lock (waiting, with a line that names the holder, when another
     heavy run holds it), records its holder in the lock file, and forks a sentinel that keeps the lock until the
     holder's process ends. The guard itself then exits 0 so the recipe body runs.

Every decision is one JSON line in the state directory (tag NEVR.HEAVY) and one line on standard error. A record that
cannot be written is reported on standard error; it does not change the decision.
"""

import datetime
import fcntl
import json
import os
import sys
import time

OPT_IN = "--i-know-exactly-what-im-doing"
LOCK_NAME = "nevr-runtime-heavy.lock"
REFUSAL = "{recipe} is a heavy build and is not run from a Claude session. Run the one test you changed instead."


def state_file():
    base = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(base, "nevr-runtime", "heavy-guard.jsonl")


def lock_file():
    return os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", LOCK_NAME)


def record(decision, recipe, **fields):
    rec = {
        "ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds"),
        "tag": "NEVR.HEAVY",
        "decision": decision,
        "recipe": recipe,
        "pid": os.getpid(),
        "ppid": os.getppid(),
        **fields,
    }
    print(f"heavy-guard: {decision} recipe={recipe}", file=sys.stderr, flush=True)
    path = state_file()
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(rec, sort_keys=True) + "\n")
    except OSError as err:
        print(f"heavy-guard: cannot write the decision record to {path}: {err}", file=sys.stderr, flush=True)


def ancestors():
    """The pids above this process, nearest first; None when /proc cannot say."""
    pids = []
    pid = os.getppid()
    while pid > 1 and len(pids) < 1000:
        pids.append(pid)
        try:
            with open(f"/proc/{pid}/stat", encoding="utf-8") as fh:
                text = fh.read()
        except OSError:
            return None
        pid = int(text[text.rindex(")") + 2:].split()[1])
    return pids


def read_holder_text():
    try:
        with open(lock_file(), encoding="utf-8") as fh:
            return fh.read().strip()
    except OSError:
        return ""


def holder_pid(text):
    for part in text.split():
        if part.startswith("pid="):
            try:
                return int(part[len("pid="):])
            except ValueError:
                return None
    return None


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        return _state(pid) != "Z"
    except OSError:
        return False


def _state(pid):
    with open(f"/proc/{pid}/stat", encoding="utf-8") as fh:
        text = fh.read()
    return text[text.rindex(")") + 2:].split()[0]


def acquire(recipe, may_wait):
    """Take the lock; return (fd, waited). Waiting is refused when the caller cannot tell it is not nested."""
    fd = os.open(lock_file(), os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return fd, False
    except BlockingIOError:
        pass
    holder = read_holder_text() or "another heavy run"
    if not may_wait:
        os.close(fd)
        print(
            f"heavy-guard: {recipe} waits for {holder}, and this process cannot tell whether it is inside that run "
            "(no /proc to read the ancestry from); refusing rather than deadlocking",
            file=sys.stderr,
        )
        return None, False
    print(f"heavy-guard: waiting for {holder}", file=sys.stderr, flush=True)
    fcntl.flock(fd, fcntl.LOCK_EX)
    return fd, True


def hold_until_exit(fd, pid_to_watch):
    """Fork a sentinel that holds the lock until pid_to_watch ends; the caller returns and exits."""
    child = os.fork()
    if child > 0:
        os.close(fd)
        return
    os.setsid()
    devnull = os.open(os.devnull, os.O_RDWR)
    for std in (0, 1, 2):
        os.dup2(devnull, std)
    while alive(pid_to_watch):
        time.sleep(0.5)
    os._exit(0)


def strip_optin(args):
    """Print the arguments without the opt-in, one per line. An argument with a line break or empty is refused (exit 1)."""
    kept = [arg for arg in args if arg != OPT_IN]
    for arg in kept:
        if not arg or "\n" in arg:
            print(f"heavy-guard: an argument is empty or has a line break: {arg!r}", file=sys.stderr)
            return 1
    for arg in kept:
        sys.stdout.write(arg + "\n")
    return 0


def guard(recipe, args):
    # just hands a variadic recipe parameter to a dependency as one space-joined argument ("a b --flag"), so the
    # opt-in is looked for among the words of the joined text, not among the arguments.
    flagged = OPT_IN in " ".join(args).split()
    claude = os.environ.get("CLAUDECODE", "") not in ("", "0")
    above = ancestors()
    holder_text = read_holder_text()
    holder = holder_pid(holder_text)
    if holder is not None and above is not None and holder in above:
        record("nested", recipe, holder=holder_text)
        return 0
    if claude and not flagged:
        record("refused", recipe, claude=True, flagged=False)
        print(REFUSAL.format(recipe=recipe), file=sys.stderr, flush=True)
        return 2
    fd, waited = acquire(recipe, may_wait=above is not None)
    if fd is None:
        record("undecided", recipe, reason="no /proc; the lock is held by another run")
        return 1
    watched = os.getppid()
    text = (
        f"recipe={recipe} pid={watched} "
        f"started={datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')} "
        f"flagged={'yes' if flagged else 'no'} claude={'yes' if claude else 'no'}\n"
    )
    os.ftruncate(fd, 0)
    os.pwrite(fd, text.encode("utf-8"), 0)
    record("acquired", recipe, claude=claude, flagged=flagged, waited=waited, holder=text.strip())
    hold_until_exit(fd, watched)
    return 0


def main(argv):
    if len(argv) < 2:
        print("usage: heavy_guard.py <recipe> [args...] | strip-optin [args...]", file=sys.stderr)
        return 1
    if argv[1] == "strip-optin":
        return strip_optin(argv[2:])
    return guard(argv[1], argv[2:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
