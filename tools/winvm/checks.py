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
        if exit_code is None:
            return Result("process_alive", FAIL, "game exited without a valid current-run exit marker")
        return Result("process_alive", PASS, f"game exited (exit code {exit_code})")
    raise ValueError(f"expect must be 'alive' or 'exit', got {expect!r}")


def check_process_markers(marker: str, run_id: str, alive: bool) -> tuple[Result, int | None]:
    """Accept only markers belonging to the launch being observed."""
    lines = marker.splitlines()
    if lines.count(f"{run_id} started") != 1:
        return Result("run_marker", FAIL, "current run start marker is missing or duplicated"), None
    exits = [line for line in lines if line.startswith(f"{run_id} exited rc=")]
    if alive:
        if exits:
            return Result("run_marker", FAIL, "current run has an exit marker while its process is alive"), None
        return Result("run_marker", PASS, "current run start marker confirmed"), None
    if len(exits) != 1:
        return Result("run_marker", FAIL, "current run exit marker is missing or duplicated"), None
    match = re.fullmatch(re.escape(run_id) + r" exited rc=(-?\d+)", exits[0])
    if match is None:
        return Result("run_marker", FAIL, "current run exit marker is malformed"), None
    return Result("run_marker", PASS, f"current run exit marker confirmed (rc={match[1]})"), int(match[1])


def check_window_enumeration(marker: str | None, run_id: str, dump: str, required: bool = True) -> Result:
    """Require this invocation's explicit completion marker; an empty dump is valid."""
    if not required:
        return Result("window_enumeration", WARN, "not applicable: game process exited before window inspection")
    if marker != f"{run_id} completed":
        return Result("window_enumeration", FAIL,
                      "window enumeration marker missing, stale, malformed, or failed")
    return Result("window_enumeration", PASS, f"window enumeration completed ({len(dump.splitlines())} lines)")


def check_window_dump_pid(dump: str, process_id: int) -> Result:
    observed = next((int(match[1]) for line in dump.splitlines()
                     if (match := re.match(r"^echovr pid:\s*(\d+)(?:\s|$)", line))), None)
    if observed != process_id:
        return Result("window_pid", FAIL, f"window dump is not associated with target PID {process_id}")
    return Result("window_pid", PASS, f"window dump is associated with target PID {process_id}")


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

# Known hook collisions require an exact reason and applicable runtime mode.
_KNOWN_HOOK_FAILURES = {
    "EchoVR::GetProcAddress": "N126/N128 diagnostic; duplicate MinHook target",
    "LoadLibraryW": "N127; Oculus filter is redundant on headless servers",
    "LoadLibraryExW": "N127; Oculus filter is redundant on headless servers",
}

_BOOT_HOOK_MARKER = re.compile(r"\bboot hooks installed\b(?P<tail>[^\r\n]*)", re.IGNORECASE)
_BOOT_HOOK_OK = re.compile(r"\bok=(?P<value>[^\s]*)", re.IGNORECASE)
_HOOK_FAILURE = re.compile(r"\bhook\s+failed\b(?P<tail>.*)", re.IGNORECASE)
_HOOK_FAILED = re.compile(r"\bhook\s+FAILED\s+name=(?P<name>\S+)(?P<tail>.*)", re.IGNORECASE)
_HOOK_SKIPPED = re.compile(r"\bhook\s+skipped\s+name=(?P<name>\S+)(?P<tail>.*)", re.IGNORECASE)
_HOOK_RESULT_FAILED = re.compile(r"\bhook\s+name=(?P<name>\S+)\s+result=FAILED\b", re.IGNORECASE)
_HOOK_NAME = re.compile(r"\bname=(?P<value>\S+)", re.IGNORECASE)
_HOOK_ERROR = re.compile(r"\b(?:reason|status)=(?P<value>\S+)", re.IGNORECASE)
_HOOK_SUMMARY = re.compile(r"hooks installed: (?P<ok>\d+) succeeded, (?P<failed>\d+) failed", re.IGNORECASE)
_BOOT_HOOK_NOT_INSTALLED = re.compile(
    r"\b(?P<kind>required|optional) boot hook not installed name=(?P<name>[^\s;]+)", re.IGNORECASE)
_OCULUS_STATUS = re.compile(
    r"Oculus Platform SDK blocking hooks: LoadLibraryW=(?P<w>ok|FAILED) "
    r"LoadLibraryExW=(?P<ex>ok|FAILED)\s+\((?P<detail>.*)\)", re.IGNORECASE)


def _boot_hook_result(text: str) -> Result:
    markers = list(_BOOT_HOOK_MARKER.finditer(text))
    if not markers:
        if re.search(r"\bAll hooks installed\b", text):
            return Result("hooks_installed", PASS, "historical All hooks installed marker")
        return Result("hooks_installed", FAIL, "missing current boot-hook marker")
    values: list[str] = []
    malformed = False
    for marker in markers:
        value = _BOOT_HOOK_OK.search(marker["tail"])
        if value is None or value["value"].lower() not in {"true", "false"}:
            malformed = True
        else:
            values.append(value["value"].lower())
    if malformed:
        return Result("hooks_installed", FAIL, "malformed boot-hook status marker")
    if "false" in values:
        return Result("hooks_installed", FAIL, "boot hooks installed ok=false")
    return Result("hooks_installed", PASS, "boot hooks installed ok=true")


def _is_headless_server(text: str) -> bool:
    return bool(re.search(
        r"Server mode[^\r\n]*headless|bit0_render=CLEAR\(HEADLESS\)",
        text, re.IGNORECASE))


def _known_hook_failure(name: str, tail: str, full_log: str) -> str | None:
    error_match = _HOOK_ERROR.search(tail)
    if error_match is None or error_match["value"] != "MH_ERROR_ALREADY_CREATED":
        return None
    if name == "EchoVR::GetProcAddress":
        return _KNOWN_HOOK_FAILURES[name]
    if name in {"LoadLibraryW", "LoadLibraryExW"} and _is_headless_server(full_log):
        return _KNOWN_HOOK_FAILURES[name]
    return None


def check_hooks(log: str) -> list[Result]:
    text = strip_ansi(log)
    results = [_boot_hook_result(text)]
    known: list[tuple[str, str]] = []
    diagnostic: list[str] = []
    required: list[str] = []
    classified: set[str] = set()

    for line in text.splitlines():
        if re.search(r"\bDIAG\b.*\bhook\b.*\b(?:failed|skipped)\b", line, re.IGNORECASE):
            diagnostic.append(line.strip())
            classified.add(line)
            continue

        not_installed = _BOOT_HOOK_NOT_INSTALLED.search(line)
        if not_installed:
            # The runtime's own verdict on a boot hook (initialize.cpp NoteBootHookResult). A required
            # one fails the boot; an optional one is a warning unless the hook failure line before
            # it already explained it as a known collision.
            name = not_installed["name"]
            if not_installed["kind"].lower() == "required":
                required.append(name)
            elif name not in _KNOWN_HOOK_FAILURES:
                diagnostic.append(line.strip())
            classified.add(line)
            continue

        failure = _HOOK_FAILED.search(line) or _HOOK_FAILURE.search(line)
        if failure:
            name = failure.groupdict().get("name")
            if name is None:
                name_match = _HOOK_NAME.search(failure["tail"])
                name = name_match["value"] if name_match is not None else None
            error_match = _HOOK_ERROR.search(failure["tail"])
            if name is None or error_match is None:
                required.append(name if name is not None else "hook failure missing name")
            else:
                explanation = _known_hook_failure(name, failure["tail"], text)
                if explanation:
                    known.append((name, explanation))
                else:
                    required.append(name)
            classified.add(line)

        skipped = _HOOK_SKIPPED.search(line)
        if skipped:
            required.append(skipped["name"])
            classified.add(line)

        result_failure = _HOOK_RESULT_FAILED.search(line)
        if result_failure:
            required.append(result_failure["name"])
            classified.add(line)

        oculus = _OCULUS_STATUS.search(line)
        if oculus:
            failed_names = [name for name, value in (("LoadLibraryW", oculus["w"]),
                                                      ("LoadLibraryExW", oculus["ex"]))
                            if value.upper() == "FAILED"]
            scoped = ("redundant on headless" in oculus["detail"]
                      and "OVR SDK is never loaded" in oculus["detail"]
                      and "N127" in oculus["detail"] and _is_headless_server(text))
            if failed_names and scoped:
                known.extend((name, _KNOWN_HOOK_FAILURES[name]) for name in failed_names)
            else:
                required.extend(failed_names)
            classified.add(line)

    for summary in _HOOK_SUMMARY.finditer(text):
        if int(summary["failed"]) > 0:
            required.append(f"summary:{summary['failed']}")
    if "[NEVR.PATCH] FATAL hooking init failed" in text:
        required.append("initialization")
    for line in text.splitlines():
        if line in classified or _HOOK_SUMMARY.search(line):
            continue
        if re.search(r"\b(?:hooks?|hook)\b", line, re.IGNORECASE) and re.search(
                r"\b(?:failed|failure|skipped)\b|result=FAILED", line, re.IGNORECASE):
            required.append("unrecognized:" + line.strip())

    if required:
        results.append(Result("no_unexpected_hook_failure", FAIL,
                              "required hook failure: " + ", ".join(sorted(set(required)))))
    else:
        results.append(Result("no_unexpected_hook_failure", PASS, "no unexpected hook failures"))
    for name, explanation in sorted(set(known)):
        results.append(Result("known_hook_failure", WARN, f"{name}: {explanation}"))
    for line in sorted(set(diagnostic)):
        results.append(Result("diagnostic_hook_failure", WARN, line))
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
    # Either outcome of the (optional, issue #21) config.json search ends this stage.
    ("config_loaded", r"Early config loaded from|no _local/config\.json under"),
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
