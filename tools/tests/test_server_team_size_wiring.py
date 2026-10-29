"""#1422: a server's private-match team size is wired once. The byte logic is tested by
src/runtime/tests/test_server_team_size.cpp and the two sites' bytes against the game by
tests/system/mode_patches_test.go (TestServerTeamSizeSites); these are source sensors for what neither can see:
the one call in boot.cpp, the client's skip line, the config key, the two addresses and the production writer, and
the level of the outcome line. Each check takes the text it reads, so a test can hand it a broken copy and see it
fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "src/runtime/lifecycle/boot.cpp"
INSTALL = ROOT / "src/runtime/patch/server_team_size.cpp"
ADDRESSES = ROOT / "src/runtime/hook/addresses.h"

CALL = "nevr_server_team_size::Install(g_isServer != FALSE);"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def boot_problems(boot: str) -> list[str]:
    """One call, in RunDeferredRuntimeBootstrap, outside any `if`, so both modes reach it and a client logs."""
    code = strip_comments(boot)
    if code.count(CALL) != 1:
        return [f"expected exactly one `{CALL}` in boot.cpp, found {code.count(CALL)}"]
    start = code.index(CALL)
    body = code.find("void RunDeferredRuntimeBootstrap(")
    if body < 0 or body > start:
        return ["the Install call is not in RunDeferredRuntimeBootstrap"]
    depth = 0
    for ch in code[code.index("{", body):start]:
        depth += ch == "{"
        depth -= ch == "}"
    if depth != 1:
        return [f"the Install call is nested {depth - 1} blocks deep; a client would not log its skip line"]
    return []


def install_problems(install: str) -> list[str]:
    code = strip_comments(install)
    problems = []
    skip = re.search(r"if\s*\(\s*!isServer\s*\)\s*\{(.*?)\breturn;\s*\}", code, flags=re.S)
    if not skip or "[NEVR.PATCH] server team size not applied: this is a client" not in skip.group(1):
        problems.append("a client does not log the skip line and return before touching the sites")
    if 'NevrCfgGetFlat("nevr_max_team_size")' not in code:
        problems.append("the size is not read from nevr_max_team_size")
    for name in ("SERVER_ASSIGN_TEAM_CAP", "SERVER_ASSIGN_TEAM_SLOT"):
        if not re.search(rf"base\s*\+\s*nevr_patch_addresses::{name}\b", code):
            problems.append(f"{name} is not the site handed to Apply")
    if not re.search(r"Apply\(\s*capSite\s*,\s*slotSite\s*,\s*size\s*,\s*ProcessWrite\s*\)", code):
        problems.append("Apply is not called with the cap site, the slot site, the size and ProcessWrite")
    if not re.search(r"return\s+ProcessMemcpy\(\s*dest\s*,\s*src\s*,\s*n\s*\)", code):
        problems.append("ProcessWrite does not write through ProcessMemcpy")
    if not re.search(r"IsFailure\(\s*outcome\s*\)\s*\?\s*EchoVR::LogLevel::Warning\s*:\s*EchoVR::LogLevel::Info", code):
        problems.append("the outcome line is not a Warning when the size asked for is not in effect")
    return problems


def address_problems(addresses: str) -> list[str]:
    code = strip_comments(addresses)
    want = {"SERVER_ASSIGN_TEAM_CAP": "0x1AB8C2", "SERVER_ASSIGN_TEAM_SLOT": "0x1ABC8A"}
    problems = []
    for name, rva in want.items():
        if not re.search(rf"constexpr\s+uintptr_t\s+{name}\s*=\s*{rva}\s*;", code):
            problems.append(f"{name} is not {rva} (ServerAssignTeam, echovr.exe 0x140000000 + RVA)")
    return problems


class ServerTeamSizeWiring(unittest.TestCase):
    def test_the_tree(self) -> None:
        self.assertEqual(boot_problems(BOOT.read_text(encoding="utf-8")), [])
        self.assertEqual(install_problems(INSTALL.read_text(encoding="utf-8")), [])
        self.assertEqual(address_problems(ADDRESSES.read_text(encoding="utf-8")), [])

    def test_a_missing_or_doubled_or_gated_call_is_found(self) -> None:
        boot = BOOT.read_text(encoding="utf-8")
        self.assertTrue(boot_problems(boot.replace(CALL, "")))
        self.assertTrue(boot_problems(boot.replace(CALL, CALL + "\n  " + CALL)))
        self.assertTrue(boot_problems(boot.replace(CALL, "if (g_isServer) { " + CALL + " }")))
        self.assertTrue(boot_problems(boot.replace(CALL, "// " + CALL)))

    def test_a_broken_install_is_found(self) -> None:
        install = INSTALL.read_text(encoding="utf-8")
        breaks = [
            ("this is a client", "this is a server"),
            ('"nevr_max_team_size"', '"nevr_team_size"'),
            ("nevr_patch_addresses::SERVER_ASSIGN_TEAM_SLOT", "nevr_patch_addresses::SERVER_ASSIGN_TEAM_CAP"),
            ("size, ProcessWrite)", "size, nullptr)"),
            ("return ProcessMemcpy(dest, src, n);", "return true;"),
            ("EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info", "EchoVR::LogLevel::Info : EchoVR::LogLevel::Info"),
        ]
        for old, new in breaks:
            with self.subTest(old=old):
                self.assertIn(old, install)
                self.assertTrue(install_problems(install.replace(old, new)))

    def test_a_moved_address_is_found(self) -> None:
        addresses = ADDRESSES.read_text(encoding="utf-8")
        self.assertTrue(address_problems(addresses.replace("= 0x1AB8C2;", "= 0x1AB8C3;")))
        self.assertTrue(address_problems(addresses.replace("= 0x1ABC8A;", "= 0x1ABC8B;")))


if __name__ == "__main__":
    unittest.main()
