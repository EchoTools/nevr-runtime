#!/usr/bin/env python3
"""A release BugSplat64.dll or nevr-runtime.dll shall not carry the scenario-test control endpoint or the map probes.

The endpoint (src/runtime/scenario/scenario_control.cpp) can inject messages into a live game
session. It exists only in the mingw-scenario preset (CMake option NEVR_SCENARIO_CONTROL). Every
log line it writes starts with "[NEVR.SCENARIO]", so that string in a DLL means the endpoint was
compiled in. See docs/design/2026-10-01-social-scenario-harness.md.

The custom-map probes (src/runtime/probe/, CMake option NEVR_MAP_PROBES) hook the game's level enable/disable and write the
session's map pin; every line they write starts with "[NEVR.PROBE]", so that string in a DLL means a probe was compiled in.

Usage: verify_scenario_control_absent.py <path to a DLL> [<path to another DLL> ...]
The runtime is where the endpoint is compiled; the host is checked too, so it stays absent from both.
Exit 0 = absent from every DLL. Exit 1 = present in one, or a DLL is missing (a gate that cannot see the artifact
does not pass).
"""
import pathlib
import sys

MARKERS = (
    (b"[NEVR.SCENARIO]", "the scenario control endpoint", "NEVR_SCENARIO_CONTROL"),
    (b"[NEVR.PROBE]", "the custom-map probes", "NEVR_MAP_PROBES"),
)


def check(path: pathlib.Path) -> tuple[bool, str]:
    if not path.is_file():
        return False, f"scenario-control: FAIL {path} does not exist; build it before this check"
    data = path.read_bytes()
    for marker, what, option in MARKERS:
        hits = data.count(marker)
        if hits:
            return False, (f"scenario-control: FAIL {path} carries {what} "
                           f"({hits} occurrence(s) of {marker.decode()}); release builds must not "
                           f"(configure without {option})")
    return True, f"scenario-control: OK {path} carries no scenario control endpoint and no map probe"


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
