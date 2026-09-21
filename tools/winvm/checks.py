"""Pure pass/fail logic for the Windows-VM system test.

No I/O and no third-party imports, so `just verify` can unit-test it
(tools/tests/test_winvm_checks.py) without a VM. Everything the harness
observes on the guest is reduced to plain text/values and judged here.

Each check returns a Result. FAIL means the runtime is broken on native
Windows; WARN means a known, already-tracked defect that must stay visible
but must not turn the run red.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

PASS, FAIL, WARN = "PASS", "FAIL", "WARN"

_ANSI = re.compile(r"\x1b\[[0-9;]*m")


@dataclass(frozen=True)
class Result:
    name: str
    status: str
    detail: str


def strip_ansi(text: str) -> str:
    return _ANSI.sub("", text)


# --- Process state --------------------------------------------------------------

def check_process_alive(alive: bool, exit_code: int | None, expect: str = "alive") -> Result:
    if expect == "alive":
        if alive:
            return Result("process_alive", PASS, "game still running at the end of the window")
        rc = "unknown" if exit_code is None else str(exit_code)
        return Result("process_alive", FAIL, f"game is not running (exit code {rc})")
    if expect == "exit":
        if alive:
            return Result("process_alive", FAIL, "game was expected to exit but is still running")
        return Result("process_alive", PASS, f"game exited (exit code {exit_code})")
    raise ValueError(f"expect must be 'alive' or 'exit', got {expect!r}")


# --- Blocking dialogs ---------------------------------------------------------
#
# A game blocked on a modal MessageBox looks exactly like a hang from outside:
# alive, Responding=True, ~0 CPU, no sockets, log silent. The first version of
# this test mistook one for a `getaddrinfo` hang. Window enumeration from inside
# the interactive session is what tells them apart, so it is a first-class check.

_TOP = re.compile(r"^TOP hwnd=\S+ class=(?P<cls>\S+) visible=(?P<vis>\w+) title=\[(?P<title>.*)\]$")
_CHILD = re.compile(r"^\s+child class=(?P<cls>\S+) text=\[(?P<text>.*)\]$")
_DIALOG_CLASS = "#32770"


def dialogs_from_window_dump(dump: str) -> list[dict]:
    """Parse the guest's window dump into [{title, texts}] for top-level dialogs."""
    dialogs: list[dict] = []
    current: dict | None = None
    for line in dump.splitlines():
        top = _TOP.match(line)
        if top:
            current = None
            if top["cls"] == _DIALOG_CLASS:
                current = {"title": top["title"], "texts": []}
                dialogs.append(current)
            continue
        child = _CHILD.match(line)
        if child and current is not None and child["cls"] == "Static" and child["text"]:
            current["texts"].append(child["text"])
    return dialogs


def check_no_modal_dialog(window_dump: str) -> Result:
    dialogs = dialogs_from_window_dump(window_dump)
    if not dialogs:
        return Result("no_modal_dialog", PASS, "no dialog windows owned by the game")
    shown = "; ".join(f"[{d['title']}] {' | '.join(d['texts'])}" for d in dialogs)
    return Result("no_modal_dialog", FAIL, f"game is blocked on a dialog: {shown}")


# --- Fatal exits ---------------------------------------------------------------

_FATAL = re.compile(r"\[FATAL\]|\[NEVR\.FATAL\]|ForceFatalExit")


def check_no_fatal(log: str, exit_code: int | None) -> Result:
    text = strip_ansi(log)
    hits = [ln.strip() for ln in text.splitlines() if _FATAL.search(ln)]
    if hits:
        return Result("no_fatal", FAIL, hits[0])
    if exit_code is not None and exit_code != 0:
        return Result("no_fatal", FAIL, f"process exited rc={exit_code} with no fatal line logged")
    return Result("no_fatal", PASS, "no fatal line in the runtime log")


# --- Hook installation ---------------------------------------------------------

# Defects that are already tracked and must not fail the run, but must keep
# printing. Add an entry only with the reason and where it is tracked.
KNOWN_HOOK_FAILURES = {
    # Native Windows and Wine both hit this; the DLL-load hook and the
    # GetProcAddress hook both create the same MinHook target.
    "EchoVR::GetProcAddress": "MH_ERROR_ALREADY_CREATED (N126/N128 diagnostic; hook is redundant)",
    # The runtime itself logs these as "redundant on headless - OVR SDK is never loaded; N127".
    "LoadLibraryW": "MH_ERROR_ALREADY_CREATED (N127; redundant on headless)",
    "LoadLibraryExW": "MH_ERROR_ALREADY_CREATED (N127; redundant on headless)",
}

_HOOK_FAILED = re.compile(r"hook FAILED name=(?P<name>\S+)")


def check_hooks(log: str) -> list[Result]:
    text = strip_ansi(log)
    results: list[Result] = []
    if "All hooks installed" not in text:
        results.append(Result("hooks_installed", FAIL, "never logged 'All hooks installed'"))
    else:
        results.append(Result("hooks_installed", PASS, "All hooks installed"))
    unexpected, known = [], []
    for m in _HOOK_FAILED.finditer(text):
        (known if m["name"] in KNOWN_HOOK_FAILURES else unexpected).append(m["name"])
    if unexpected:
        results.append(Result("no_unexpected_hook_failure", FAIL,
                              "hook FAILED: " + ", ".join(sorted(set(unexpected)))))
    else:
        results.append(Result("no_unexpected_hook_failure", PASS, "no unexpected hook failures"))
    for name in sorted(set(known)):
        results.append(Result("known_hook_failure", WARN, f"{name}: {KNOWN_HOOK_FAILURES[name]}"))
    return results


# --- Engine progress -------------------------------------------------------------

# Stages a healthy server boot reaches, in order (regexes over the runtime log).
# The last one seen tells you where a stalled run stopped. `broadcaster` is the
# point issue #13 stalls before: CBroadcaster::Initialize (which calls
# getaddrinfo) must have returned for Listen to be entered. It only shows up in
# the ~30 s periodic hook_liveness report, so the observation window has to
# cover that.
ENGINE_STAGES = (
    ("banner", r"Echo VR\n"),
    ("config_loaded", r"Early config loaded from"),
    ("sysnet", r"\[SYSNET\] Found Internet connection"),
    ("broadcaster", r"hook_liveness name=CBroadcaster::Listen entries=[1-9]\d* entered=yes"),
)


def engine_stage_reached(log: str) -> str | None:
    text = strip_ansi(log)
    reached = None
    for name, pattern in ENGINE_STAGES:
        if re.search(pattern, text):
            reached = name
    return reached


def check_engine_progress(log: str, required: str) -> Result:
    names = [n for n, _ in ENGINE_STAGES]
    if required not in names:
        raise ValueError(f"unknown stage {required!r}; known: {names}")
    reached = engine_stage_reached(log)
    if reached is not None and names.index(reached) >= names.index(required):
        return Result("engine_progress", PASS, f"reached '{reached}' (required '{required}')")
    return Result("engine_progress", FAIL,
                  f"stopped at '{reached or 'nothing'}', never reached '{required}'")


# --- getaddrinfo probe ---------------------------------------------------------------

_PROBE = re.compile(
    r'^(?P<label>game call: node="" flags=0)\s+rc=(?P<rc>-?\d+) wsaerr=\d+ elapsed=(?P<ms>[\d.]+) ms'
)


def check_getaddrinfo(probe_output: str, max_ms: float = 2000.0) -> Result:
    """The exact call CBroadcaster::Initialize makes (issue #13), timed natively."""
    rows = [m for m in (_PROBE.match(ln.strip()) for ln in probe_output.splitlines()) if m]
    if not rows:
        return Result("getaddrinfo_fast", FAIL, "probe printed no 'game call' rows")
    bad_rc = [r for r in rows if int(r["rc"]) != 0]
    if bad_rc:
        return Result("getaddrinfo_fast", FAIL, f"getaddrinfo failed rc={bad_rc[0]['rc']}")
    worst = max(float(r["ms"]) for r in rows)
    if worst > max_ms:
        return Result("getaddrinfo_fast", FAIL, f"slowest call {worst:.1f} ms > {max_ms:.0f} ms")
    return Result("getaddrinfo_fast", PASS, f"{len(rows)} calls, slowest {worst:.1f} ms")


def overall(results: list[Result]) -> bool:
    return all(r.status != FAIL for r in results)
