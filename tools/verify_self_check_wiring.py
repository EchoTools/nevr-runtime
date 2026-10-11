#!/usr/bin/env python3
"""Fail `just verify` when the self-checks stop being unconditional or the runtime adds a debug query (#451).

Decided (Spritz, 2026-10-11): self-checks are on in every build, so nothing in a build may behave differently
because of how it was stamped, and the runtime never adds `debug=true` to a connection by build type: whether a
player sends the FULL remote logs is the game service's decision (profile.EnableAllRemoteLogs,
serviceSettings.EnableSessionDebug). This gate keeps both true:

* the PC bridge turns the unit on unconditionally (WireSelfChecks) and the Quest bridge wires it through
  ApplySelfCheck with no feature in between;
* no switch by build type or feature exists (no NEVR_SELF_CHECKS define, no Quest `self_check` feature) and
  nothing appends a debug query (no AppendRemoteDebugParam / WantsRemoteDebug / remoteDebugQuery, no
  `debug=true` literal in the runtime sources).

Source checks, because the connect path itself runs only in a live session.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# (file, regex that must match, message)
REQUIRED = [
    ("src/runtime/compat/ws_bridge.cpp",
     r"static void WireSelfChecks\(\)\s*\{\s*nevr_self_check::SetEnabled\(true\);",
     "WireSelfChecks does not turn the unit on as its first, unconditional statement"),
    ("src/runtime/compat/ws_bridge.cpp", r"WireSelfChecks\(\);",
     "InstallWebSocketBridge does not wire the self-check sender"),
    ("src/quest/integration/production_steps.cpp", r"ApplySelfCheck\(\s*&config\.tap\s*,",
     "the Quest bridge config is not wired through ApplySelfCheck"),
    ("src/quest/integration/self_check_wiring.cpp", r"nevr_self_check::SetEnabled\(true\);",
     "ApplySelfCheck does not turn the unit on"),
]

# Tokens that must not appear in the runtime sources: a switch by build type or feature, or a debug query.
FORBIDDEN_TOKENS = [
    "AppendRemoteDebugParam",
    "WantsRemoteDebug",
    "remoteDebugQuery",
    "NEVR_SELF_CHECKS",
    "nevr_self_checks_by_stamp",
    "Feature::kSelfCheck",
    "Features::selfCheck",
    "effective.selfCheck",
    "requested.selfCheck",
    "bool selfCheck",
]
FORBIDDEN_REGEX = [
    (re.compile(r"\bdebug=true\b"), "a `debug=true` query literal", False,
     "the game service decides who sends the full remote logs"),
    (re.compile(r"\{\s*\"debug\"\s*,"), "a `debug` query parameter being appended", False,
     "the game service decides who sends the full remote logs"),
    # the unit stays on once the bridge turned it on; a test may switch it off
    (re.compile(r"nevr_self_check::SetEnabled\(\s*false\s*\)"), "a later SetEnabled(false)", True,
     "self-checks are on in every build"),
]
SCAN_SUFFIXES = {".cpp", ".h", ".cmake", ".txt", ".in"}
SCAN_ROOTS = ["src", "cmake", "CMakeLists.txt", "justfile"]


def scan_files(root: Path):
    for entry in SCAN_ROOTS:
        base = root / entry
        if base.is_file():
            yield base
        elif base.is_dir():
            for path in sorted(base.rglob("*")):
                if path.is_file() and (path.suffix in SCAN_SUFFIXES or path.name == "justfile"):
                    yield path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO)
    args = parser.parse_args()
    failed = 0
    for rel, pattern, message in REQUIRED:
        path = args.root / rel
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            print(f"verify: FAIL — self-check wiring: cannot read {rel}: {error}", file=sys.stderr)
            failed += 1
            continue
        if not re.search(pattern, text, re.S):
            print(f"verify: FAIL — self-check wiring: {rel}: {message}", file=sys.stderr)
            failed += 1
    scanned = 0
    for path in scan_files(args.root):
        scanned += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(args.root).as_posix()
        for token in FORBIDDEN_TOKENS:
            if token in text:
                print(f"verify: FAIL — self-check wiring: {rel} names `{token}`: self-checks are on in every build "
                      "and the runtime adds no debug query by build type (#451)", file=sys.stderr)
                failed += 1
        for regex, what, tests_may, reason in FORBIDDEN_REGEX:
            if tests_may and "/tests/" in "/" + rel:
                continue
            if regex.search(text):
                print(f"verify: FAIL — self-check wiring: {rel} has {what}: {reason} (#451)", file=sys.stderr)
                failed += 1
    if scanned == 0:
        print("verify: FAIL — self-check wiring: nothing was scanned", file=sys.stderr)
        return 1
    if failed:
        return 1
    print(f"verify: self-check wiring OK ({len(REQUIRED)} required, {scanned} files scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
