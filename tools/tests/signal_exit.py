"""Signal a child a test started and record how long it takes to exit (#1247).

A shell starts a background job (nohup, &) with SIGINT and SIGHUP ignored, every child inherits that, and neither a
non-interactive bash nor Python can take over a signal that was ignored when it started: bash cannot trap it, and
Python installs no KeyboardInterrupt handler for an ignored SIGINT. A test that signals its child to prove the child's
handler then waits out its whole budget. The record this writes says which happened: a slow exit, or a signal the
child never saw."""
from __future__ import annotations

import json
import pathlib
import signal
import subprocess
import sys
import time
import unittest


def default_signals(*sigs: signal.Signals):
    """A preexec_fn that puts each of `sigs` back to its default disposition in the child before exec, so the child
    can install its own handler whatever this test run inherited."""
    def reset() -> None:
        for sig in sigs:
            signal.signal(sig, signal.SIG_DFL)
    return reset


def ignores(pid: int, sig: signal.Signals) -> bool:
    """Whether process `pid` ignores `sig`, from the kernel's own account (/proc/<pid>/status SigIgn)."""
    status = dict(line.split(":\t", 1) for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines()
                  if line.startswith("SigIgn"))
    return bool(int(status["SigIgn"], 16) & (1 << (sig - 1)))


def signal_to_exit(case: unittest.TestCase, proc: subprocess.Popen, sig: signal.Signals, bound_s: float) -> dict:
    """Send `sig` to `proc` and wait up to `bound_s` seconds for it to exit. One JSON record (`event`
    `signal_to_exit`) goes to stderr, which pytest keeps with a failure and `-rP` shows for a pass: the test, the
    signal, whether the child ignored it when it was sent, the time from the signal to the exit, the bound and the
    result (`exited` or `timeout`). A timeout fails `case` with the record and, when the child ignored the signal,
    says that its handler never ran. Returns the record."""
    ignored = ignores(proc.pid, sig)
    start = time.monotonic()
    proc.send_signal(sig)
    try:
        proc.communicate(timeout=bound_s)
        result = "exited"
    except subprocess.TimeoutExpired:
        result = "timeout"
    record = {"event": "signal_to_exit", "test": case.id(), "signal": sig.name, "child_ignores_signal": ignored,
              "elapsed_ms": round((time.monotonic() - start) * 1000, 1), "bound_s": bound_s, "result": result,
              "returncode": proc.returncode}
    sys.stderr.write(json.dumps(record) + "\n")
    if result == "timeout":
        cause = f": the child ignores {sig.name}, so its handler never ran" if ignored else ""
        case.fail(f"the child did not exit within {bound_s} s of {sig.name}{cause}; {json.dumps(record)}")
    return record
