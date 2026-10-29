"""#1423: AI bots on a dedicated server are wired once. The pure logic is tested by
src/runtime/tests/test_server_bots.cpp and the sites' bytes against the game by tests/system/mode_patches_test.go
(TestServerBotsSites); these are source sensors for what neither can see: the one Install call in boot.cpp, the one
OnFrame call in the per-frame dispatcher (a server, the game-loop thread), the one session-start rewrite before the
forward to the game, the client's skip line, the config key, the witness check before arming, the offline bit put back
after the call, and the three addresses. Each check takes the text it reads, so a test can hand it a broken copy and
see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "src/runtime/lifecycle/boot.cpp"
TICK = ROOT / "src/runtime/frame/tick.cpp"
CALLBACKS = ROOT / "src/runtime/server/gameserver_callbacks.cpp"
DRIVER = ROOT / "src/runtime/patch/server_bots.cpp"
ADDRESSES = ROOT / "src/runtime/hook/addresses.h"

INSTALL_CALL = "nevr_server_bots::Install(g_isServer != FALSE);"
FRAME_CALL = "nevr_server_bots::OnFrame();"
FRAME_GATE = "if (nevr_server_bots::DriverRuns(g_isServer != FALSE, loop, GetCurrentThreadId())) " + FRAME_CALL
SETTINGS_CALL = "nevr_server_bots::SessionSettings(&data, &size);"
FORWARD = "EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyStartSessionV4,"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def depth_in(code: str, function: str, at: int) -> int | None:
    """The brace depth of `at` inside `function`'s body (1 = the body itself), or None when it is not inside it."""
    start = code.find(function)
    if start < 0 or start > at:
        return None
    depth = 0
    for ch in code[code.index("{", start):at]:
        depth += ch == "{"
        depth -= ch == "}"
        if depth == 0:
            return None
    return depth


def boot_problems(boot: str) -> list[str]:
    """One call, in RunDeferredRuntimeBootstrap, outside any `if`, so both modes reach it and a client logs."""
    code = strip_comments(boot)
    if code.count(INSTALL_CALL) != 1:
        return [f"expected exactly one `{INSTALL_CALL}` in boot.cpp, found {code.count(INSTALL_CALL)}"]
    depth = depth_in(code, "void RunDeferredRuntimeBootstrap(", code.index(INSTALL_CALL))
    if depth is None:
        return ["the Install call is not in RunDeferredRuntimeBootstrap"]
    if depth != 1:
        return [f"the Install call is nested {depth - 1} blocks deep; a client would not log its skip line"]
    return []


def tick_problems(tick: str) -> list[str]:
    """One call, in DispatchPerFrameWork (N86/N110: no second dispatcher), on a server and the game-loop thread."""
    code = strip_comments(tick)
    if code.count(FRAME_CALL) != 1:
        return [f"expected exactly one `{FRAME_CALL}` in tick.cpp, found {code.count(FRAME_CALL)}"]
    if FRAME_GATE not in code:
        return ["OnFrame is not gated on a server and the game-loop thread (it calls into the game's net game)"]
    if depth_in(code, "void DispatchPerFrameWork(", code.index(FRAME_CALL)) is None:
        return ["the OnFrame call is not in DispatchPerFrameWork"]
    return []


def callbacks_problems(callbacks: str) -> list[str]:
    """One rewrite, in the legacy session-start branch, before the frame is forwarded to the game."""
    code = strip_comments(callbacks)
    if code.count(SETTINGS_CALL) != 1:
        return [f"expected exactly one `{SETTINGS_CALL}`, found {code.count(SETTINGS_CALL)}"]
    at = code.index(SETTINGS_CALL)
    branch = code.rfind("else if (msgId == SYM_LEGACY_SESSION_START)", 0, at)
    nxt = code.find("else if (msgId ==", branch + 1)
    if branch < 0 or (0 <= nxt < at):
        return ["the rewrite is not in the SYM_LEGACY_SESSION_START branch"]
    fwd = code.find(FORWARD, at)
    if fwd < 0 or (0 <= nxt < fwd):
        return ["the rewritten frame is not the one forwarded to the game"]
    return []


def driver_problems(driver: str) -> list[str]:
    code = strip_comments(driver)
    problems = []
    skip = re.search(r"if\s*\(\s*!isServer\s*\)\s*\{(.*?)\breturn;\s*\}", code, flags=re.S)
    if not skip or "[NEVR.GAMESERVER] server bots not applied: this is a client" not in skip.group(1):
        problems.append("a client does not log the skip line and return")
    if 'NevrCfgGetFlat("nevr_bots_per_session")' not in code:
        problems.append("the count is not read from nevr_bots_per_session")
    check = re.search(r"FirstMismatch\(\s*base\s*\)", code)
    arm = code.find("g_armed.store(wanted")
    if not check or arm < 0 or check.start() > arm:
        problems.append("the sites are not checked before the driver is armed")
    elif not re.search(r"if\s*\(\s*bad\s*!=\s*kWitnessCount\s*\)\s*\{[^}]*?return;", code[check.start():arm], flags=re.S):
        problems.append("a site mismatch does not leave the driver off")
    for name in ("REQUEST_ADD_BOT", "FIND_POOL", "NO_SPAWN_POSITION"):
        if not re.search(rf"base\s*\+\s*nevr_patch_addresses::{name}\b", code):
            problems.append(f"{name} is not read from the game image")
    call = code.find("requestAddBot(netGame, request.bytes)")
    set_bit = code.find("*hostingFlags = saved | kHostingFlagOffline;")
    put_back = code.find("*hostingFlags = saved;")
    if not (0 <= set_bit < call < put_back):
        problems.append("the offline bit is not set around the RequestAddBot call and put back after it")
    if not re.search(r"BodyFree\(\s*body\s*,\s*2\s*\)", code) or code.find("BodyFree(") > call:
        problems.append("the body pools are not checked before the request")
    if not re.search(r"IsSettingsFailure\(\s*r\.result\s*\)\s*\?\s*EchoVR::LogLevel::Warning\s*:\s*EchoVR::LogLevel::Info", code):
        problems.append("a session start that cannot be read is not a Warning")
    return problems


def address_problems(addresses: str) -> list[str]:
    code = strip_comments(addresses)
    want = {"REQUEST_ADD_BOT": "0x1A0D70", "FIND_POOL": "0x502D80", "NO_SPAWN_POSITION": "0x201D338"}
    return [
        f"{name} is not {rva} (echovr.exe 0x140000000 + RVA)"
        for name, rva in want.items()
        if not re.search(rf"constexpr\s+uintptr_t\s+{name}\s*=\s*{rva}\s*;", code)
    ]


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class ServerBotsWiring(unittest.TestCase):
    def test_the_tree(self) -> None:
        self.assertEqual(boot_problems(read(BOOT)), [])
        self.assertEqual(tick_problems(read(TICK)), [])
        self.assertEqual(callbacks_problems(read(CALLBACKS)), [])
        self.assertEqual(driver_problems(read(DRIVER)), [])
        self.assertEqual(address_problems(read(ADDRESSES)), [])

    def test_a_missing_doubled_or_gated_install_is_found(self) -> None:
        boot = read(BOOT)
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, "")))
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, INSTALL_CALL + "\n  " + INSTALL_CALL)))
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, "if (g_isServer) { " + INSTALL_CALL + " }")))
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, "// " + INSTALL_CALL)))

    def test_an_ungated_or_moved_frame_call_is_found(self) -> None:
        tick = read(TICK)
        self.assertTrue(tick_problems(tick.replace(FRAME_GATE, "")))
        self.assertTrue(tick_problems(tick.replace("DriverRuns(g_isServer != FALSE,", "DriverRuns(true,")))
        self.assertTrue(tick_problems(tick.replace("loop, GetCurrentThreadId())) " + FRAME_CALL, "loop, loop)) " + FRAME_CALL)))
        self.assertTrue(tick_problems(tick.replace(FRAME_GATE, "") + "\nvoid Other() { " + FRAME_GATE + " }\n"))

    def test_a_rewrite_after_the_forward_or_elsewhere_is_found(self) -> None:
        callbacks = read(CALLBACKS)
        self.assertTrue(callbacks_problems(callbacks.replace(SETTINGS_CALL, "")))
        moved = callbacks.replace(SETTINGS_CALL, "").replace(
            '"SNSLobbyStartSessionv4", data, size);', '"SNSLobbyStartSessionv4", data, size);\n' + SETTINGS_CALL, 1)
        self.assertTrue(callbacks_problems(moved))

    def test_a_broken_driver_is_found(self) -> None:
        driver = read(DRIVER)
        breaks = [
            ("this is a client", "this is a server"),
            ('"nevr_bots_per_session"', '"nevr_bots"'),
            ("if (bad != kWitnessCount) {", "if (false) {"),
            ("nevr_patch_addresses::NO_SPAWN_POSITION", "nevr_patch_addresses::FIND_POOL"),
            ("*hostingFlags = saved;", "(void)saved;"),
            ("if (!BodyFree(body, 2)) {", "if (false) {"),
            ("EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,\n      \"[NEVR.GAMESERVER] server bots session",
             "EchoVR::LogLevel::Info : EchoVR::LogLevel::Info,\n      \"[NEVR.GAMESERVER] server bots session"),
        ]
        for old, new in breaks:
            with self.subTest(old=old):
                self.assertIn(old, driver)
                self.assertTrue(driver_problems(driver.replace(old, new)))

    def test_a_moved_address_is_found(self) -> None:
        addresses = read(ADDRESSES)
        for old, new in (("= 0x1A0D70;", "= 0x1A0D71;"), ("= 0x502D80;", "= 0x502D81;"), ("= 0x201D338;", "= 0x201D33C;")):
            with self.subTest(old=old):
                self.assertTrue(address_problems(addresses.replace(old, new)))


class TheTestIsBuiltAndRun(unittest.TestCase):
    """test-auth-unit builds and runs test_server_bots, so the GTests above are part of the quick check."""

    def test_cmake_builds_it(self) -> None:
        cmake = strip_comments(read(ROOT / "src/runtime/CMakeLists.txt").replace("#", "//"))
        self.assertIn("add_executable(test_server_bots tests/test_server_bots.cpp)", cmake)
        self.assertIn("gtest_discover_tests(test_server_bots", cmake)

    def test_the_unit_recipe_builds_and_runs_it(self) -> None:
        justfile = read(ROOT / "justfile")
        self.assertRegex(justfile, r"--target test_server_bots\b")
        self.assertRegex(justfile, r"for test_name in [^\n]*\btest_server_bots\b")


if __name__ == "__main__":
    unittest.main()
