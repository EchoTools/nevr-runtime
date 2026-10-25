#!/usr/bin/env python3
"""A release BugSplat64.dll shall not carry the scenario-test control endpoint.

The endpoint (src/runtime/scenario/scenario_control.cpp) can inject messages into a live game
session. It exists only in the mingw-scenario preset (CMake option NEVR_SCENARIO_CONTROL). Every
log line it writes starts with "[NEVR.SCENARIO]", so that string in a DLL means the endpoint was
compiled in. See docs/design/2026-10-01-social-scenario-harness.md.

Usage: verify_scenario_control_absent.py <path to BugSplat64.dll>
Exit 0 = absent. Exit 1 = present, or the DLL is missing (a gate that cannot see the artifact
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
    if len(argv) != 2:
        print("usage: verify_scenario_control_absent.py <BugSplat64.dll>", file=sys.stderr)
        return 1
    ok, message = check(pathlib.Path(argv[1]))
    print(message, file=sys.stdout if ok else sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
