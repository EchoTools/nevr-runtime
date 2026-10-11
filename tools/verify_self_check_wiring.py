#!/usr/bin/env python3
"""Fail `just verify` when the self-check wiring stops going through the tested functions (#451).

The PC upgrade's debug query is decided by nevr_self_check::WantsRemoteDebug (tested in test_self_check:
the login connection of a client build with the unit on, never a dedicated game server). ws_bridge.cpp
must ask it, and must append through AppendRemoteDebugParam. The Quest bridge's config must be wired
through ApplySelfCheck (tested in integration_sequence_test), and the PC unit's flag must come from the
stamped version (tools/tests/test_self_checks_flag.py). Source checks, because the connect path itself
runs only in a live session.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

CHECKS = [
    ("src/runtime/compat/ws_bridge.cpp",
     r"nevr_self_check::WantsRemoteDebug\(\s*connIdx\s*,\s*g_isServer\s*!=\s*FALSE\s*\)",
     "the login connection's debug query is not decided by nevr_self_check::WantsRemoteDebug(connIdx, g_isServer)"),
    ("src/runtime/compat/ws_bridge.cpp",
     r"nevr_serverdb_uri::AppendRemoteDebugParam\(\s*remoteUrl\s*\)",
     "the debug query is not appended through nevr_serverdb_uri::AppendRemoteDebugParam"),
    ("src/runtime/compat/ws_bridge.cpp",
     r"WireSelfChecks\(\);",
     "InstallWebSocketBridge does not wire the self-check sender"),
    ("src/quest/integration/production_steps.cpp",
     r"ApplySelfCheck\(\s*&config\.tap\s*,\s*&config\.remoteDebugQuery\s*,",
     "the Quest bridge config is not wired through ApplySelfCheck"),
    ("src/runtime/compat/self_check.h",
     r"return connIdx == 1 && !isServer && Enabled\(\);",
     "WantsRemoteDebug is not login-connection only, client only and unit-on only"),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO)
    args = parser.parse_args()
    failed = 0
    for rel, pattern, message in CHECKS:
        path = args.root / rel
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            print(f"verify: FAIL — self-check wiring: cannot read {rel}: {error}", file=sys.stderr)
            failed += 1
            continue
        if not re.search(pattern, text):
            print(f"verify: FAIL — self-check wiring: {rel}: {message}", file=sys.stderr)
            failed += 1
    if failed:
        return 1
    print(f"verify: self-check wiring OK ({len(CHECKS)} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
