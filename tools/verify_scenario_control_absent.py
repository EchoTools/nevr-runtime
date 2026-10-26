#!/usr/bin/env python3
"""A release BugSplat64.dll or nevr-runtime.dll shall not carry the scenario-test control endpoint.

The endpoint (src/runtime/scenario/scenario_control.cpp) can inject messages into a live game
session. It exists only in the mingw-scenario preset (CMake option NEVR_SCENARIO_CONTROL). Every
log line it writes starts with "[NEVR.SCENARIO]", so that string in a DLL means the endpoint was
compiled in. See docs/design/2026-10-01-social-scenario-harness.md.

Usage: verify_scenario_control_absent.py <path to a DLL> [<path to another DLL> ...]
The runtime is where the endpoint is compiled; the host is checked too, so it stays absent from both.
Exit 0 = absent from every DLL. Exit 1 = present in one, or a DLL is missing (a gate that cannot see the artifact
does not pass).
"""
import pathlib
import sys

MARKER = b"[NEVR.SCENARIO]"


def check(path: pathlib.Path) -> tuple[bool, str]:
    if not path.is_file():
        return False, f"scenario-control: FAIL {path} does not exist; build it before this check"
    data = path.read_bytes()
    hits = data.count(MARKER)
    if hits:
        return False, (f"scenario-control: FAIL {path} carries the scenario control endpoint "
                       f"({hits} occurrence(s) of {MARKER.decode()}); release builds must not "
                       "(configure without NEVR_SCENARIO_CONTROL)")
    return True, f"scenario-control: OK {path} carries no scenario control endpoint"


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: verify_scenario_control_absent.py <dll> [<dll> ...]", file=sys.stderr)
        return 1
    failed = False
    for name in argv[1:]:
        ok, message = check(pathlib.Path(name))
        print(message, file=sys.stdout if ok else sys.stderr)
        failed = failed or not ok
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
