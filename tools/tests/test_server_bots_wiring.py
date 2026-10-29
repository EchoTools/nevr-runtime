"""#1423: AI bots on a dedicated server are wired once. The decisions, the reads of the game's memory, the offline bit
and every log line are tested by src/runtime/tests/test_server_bots.cpp and the sites' bytes against the game by
tests/system/mode_patches_test.go (TestServerBotsSites); these are source sensors for what neither can see: the one
Install call in boot.cpp, the one OnFrame call in the per-frame dispatcher (a server, the game-loop thread), the one
session-start rewrite before the forward to the game, that server_bots.cpp calls those tested pieces and logs what they
give, the three addresses, and that each site the driver checks at boot is, byte for byte, a row of the Go test
that reads the pinned echovr.exe. Each check takes the text it reads, so a test can hand it a broken copy and see it
fail."""
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
HEADER = ROOT / "src/runtime/patch/server_bots.h"
GO_SITES = ROOT / "tests/system/mode_patches_test.go"

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


MODE_RETURN = re.compile(r"if\s*\(\s*!?\s*g_isServer\b[^)]*\)\s*(?:\{[^{}]*?)?\breturn\b")


def boot_problems(boot: str) -> list[str]:
    """One call, in RunDeferredRuntimeBootstrap, outside any `if`, with no return on the mode before it, so both
    modes reach it and a client logs."""
    code = strip_comments(boot)
    if code.count(INSTALL_CALL) != 1:
        return [f"expected exactly one `{INSTALL_CALL}` in boot.cpp, found {code.count(INSTALL_CALL)}"]
    at = code.index(INSTALL_CALL)
    depth = depth_in(code, "void RunDeferredRuntimeBootstrap(", at)
    if depth is None:
        return ["the Install call is not in RunDeferredRuntimeBootstrap"]
    if depth != 1:
        return [f"the Install call is nested {depth - 1} blocks deep; a client would not log its skip line"]
    if not code[:at].rstrip().endswith((";", "{", "}")):
        return ["the Install call is the body of an `if` or a loop without braces"]
    if MODE_RETURN.search(code[code.find("void RunDeferredRuntimeBootstrap("):at]):
        return ["RunDeferredRuntimeBootstrap returns on the mode before the Install call"]
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


DRIVER_NEEDS = [
    (r'text\s*=\s*isServer\s*\?\s*NevrCfgGetFlat\("nevr_bots_per_session"\)\s*:\s*nullptr\s*;',
     "the count is not read from nevr_bots_per_session on a server only"),
    (r"InstallLines\(\s*isServer\s*,\s*text\s*,", "Install does not decide through InstallLines"),
    (r"g_armed\.store\(\s*r\.armed\b", "the driver is not armed with InstallLines' count"),
    (r"for\s*\(\s*const\s+Line&\s+line\s*:\s*r\.lines\s*\)\s*LogLine\(\s*line\s*\)\s*;",
     "Install does not log every line InstallLines gives"),
    (r"line\.warning\s*\?\s*EchoVR::LogLevel::Warning\s*:\s*EchoVR::LogLevel::Info",
     "a warning line is not logged at Warning"),
    (r"ReadNetGame\(\s*netGame\s*\)", "the net game is not read through ReadNetGame"),
    (r"PendingWarningDue\(\s*g_schedule\s*,\s*gate\s*==\s*Gate::kPending\s*,\s*now\s*\)\s*\)\s*\{\s*"
     r"LogLine\(\s*PendingLine\(", "a request the game does not answer is not said"),
    (r"ReadBodyPools\(\s*reinterpret_cast<FindPoolFn>\(\s*base\s*\+\s*nevr_patch_addresses::FIND_POOL\s*\)\s*,\s*"
     r"pools\s*,\s*body\s*\)", "the body pools are not read through ReadBodyPools with the game's FindPool"),
    (r"pools\s*=\s*ReadAt<void\*>\(\s*netGame\s*,\s*kNetGamePoolsOffset\s*\)", "the pools are not the net game's"),
    (r"LogLine\(\s*NoBodyLine\(", "no free body is not said"),
    (r"WithOfflineBit\(\s*HostingFlags\(\s*lobby\s*\)\s*,\s*\[&\]\s*\{\s*return\s+"
     r"requestAddBot\(\s*netGame\s*,\s*request\.bytes\s*\)\s*!=\s*0\s*;\s*\}\s*\)",
     "the offline bit is not set around the RequestAddBot call and put back after it"),
    (r"LogLine\(\s*RequestLine\(\s*accepted\b", "the request's answer is not said"),
    (r"LogLine\(\s*SettingsLine\(", "the session-settings result is not said"),
    (r"UseRewrite\(\s*r\s*,\s*g_rewrittenFrame\s*,\s*frame\s*,\s*size\s*\)",
     "the rewritten session start is not the frame the game gets"),
    (r"RewriteSessionSettings\(\s*bytes\s*,\s*static_cast<size_t>\(\s*\*size\s*\)\s*,\s*wanted\s*,\s*"
     r"kPlayersPerTeam\s*\)", "the settings are not rewritten for the armed count"),
    (r"g_session\s*=\s*session\s*;", "the session id the lines carry is not the session start's"),
    (r"if\s*\(\s*netGame\s*==\s*nullptr\s*\)\s*return\s*;", "a missing net game is read"),
    (r"lobby\s*=\s*ReadAt<uint8_t\*>\(\s*netGame\s*,\s*kNetGameLobbyOffset\s*\)\s*;\s*"
     r"if\s*\(\s*lobby\s*==\s*nullptr\s*\)\s*return\s*;", "the lobby is not the net game's, or a missing one is written"),
    (r"Decide\(\s*g_schedule\s*,\s*wanted\s*,\s*view\.inGame\s*,\s*entrants\s*,\s*view\.pending\s*,\s*now\s*\)",
     "the schedule does not decide on the net game's fields"),
    (r"if\s*\(\s*gate\s*!=\s*Gate::kAsk\s*\)\s*return\s*;", "a request is made when the schedule says no"),
    (r"Requested\(\s*g_schedule\s*,\s*accepted\s*\)", "an accepted request is not counted"),
    (r"memcpy\(\s*noPosition\s*,\s*base\s*\+\s*nevr_patch_addresses::NO_SPAWN_POSITION\s*,\s*kNoPositionSize\s*\)\s*;"
     r"\s*const\s+Request\s+request\s*=\s*BuildRequest\(\s*noPosition\s*\)", "the request does not carry the game's no-position"),
    (r"NoBodyWarningDue\(\s*g_schedule\s*,\s*now\s*\)", "the no-body warning is not rate limited"),
    (r"server bots driver running thread=", "the driver's first frame is not said"),
    # DriverRuns gates on the mode and the thread only: an unarmed server reaches OnFrame and every session start.
    (r"void\s+OnFrame\(\)\s*\{\s*const\s+uint32_t\s+wanted\s*=\s*ArmedBots\(\)\s*;\s*if\s*\(\s*wanted\s*==\s*0\s*\)\s*return\s*;",
     "OnFrame does not stop first when no bots are armed"),
    (r"void\s+SessionSettings\([^)]*\)\s*\{\s*const\s+uint32_t\s+wanted\s*=\s*ArmedBots\(\)\s*;\s*"
     r"if\s*\(\s*wanted\s*==\s*0\s*\)\s*return\s*;", "an unarmed server's session start is rewritten"),
]


def driver_problems(driver: str) -> list[str]:
    """server_bots.cpp only: the pure decisions are in server_bots.h and its GTests; this is what calls them."""
    code = strip_comments(driver)
    problems = [why for pattern, why in DRIVER_NEEDS if not re.search(pattern, code)]
    for name in ("REQUEST_ADD_BOT", "FIND_POOL", "NO_SPAWN_POSITION"):
        if not re.search(rf"base\s*\+\s*nevr_patch_addresses::{name}\b", code):
            problems.append(f"{name} is not read from the game image")
    call = code.find("requestAddBot(netGame, request.bytes)")
    if not re.search(r"BodyFree\(\s*body\s*,\s*2\s*\)", code) or code.find("BodyFree(") > call:
        problems.append("the body pools are not checked before the request")
    if re.search(r"\*\s*hostingFlags\s*=|kHostingFlagOffline", code):
        problems.append("the driver writes the hosting flags itself, outside WithOfflineBit")
    return problems


def address_problems(addresses: str) -> list[str]:
    code = strip_comments(addresses)
    want = {"REQUEST_ADD_BOT": "0x1A0D70", "FIND_POOL": "0x502D80", "NO_SPAWN_POSITION": "0x201D338"}
    return [
        f"{name} is not {rva} (echovr.exe 0x140000000 + RVA)"
        for name, rva in want.items()
        if not re.search(rf"constexpr\s+uintptr_t\s+{name}\s*=\s*{rva}\s*;", code)
    ]


def header_witnesses(header: str) -> dict[str, tuple[int, bytes]] | str:
    """kWitnesses of server_bots.h: name -> (rva, bytes), each array and RVA constant read from the header itself."""
    code = strip_comments(header)
    arrays = {m.group(1): bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})\b", m.group(2)))
              for m in re.finditer(r"inline\s+constexpr\s+uint8_t\s+(k\w+Bytes)\[\]\s*=\s*\{([^}]*)\}", code)}
    rvas = {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r"constexpr\s+uintptr_t\s+(k\w+Rva)\s*=\s*0x([0-9a-fA-F]+)\s*;", code)}
    table = re.search(r"kWitnesses\[\]\s*=\s*\{(.*?)\n\};", code, flags=re.S)
    if not table:
        return "no kWitnesses table in server_bots.h"
    out: dict[str, tuple[int, bytes]] = {}
    for m in re.finditer(r'\{"([^"]+)",\s*(\w+),\s*(k\w+Bytes),\s*sizeof\((k\w+Bytes)\)\}', table.group(1)):
        name, rva, array, sized = m.groups()
        if array != sized or array not in arrays:
            return f"witness {name} does not name one array for its bytes and its size"
        value = int(rva, 16) if rva.startswith("0x") else rvas.get(rva)
        if value is None:
            return f"witness {name}: RVA {rva} is not a constant of the header"
        out[name] = (value, arrays[array])
    return out


def go_sites(go: str) -> dict[str, tuple[int, bytes]]:
    body = go[go.index("func TestServerBotsSites("):]
    body = body[:body.index("\n}\n")]
    return {m.group(1): (int(m.group(2), 16), bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", m.group(3))))
            for m in re.finditer(r'\{"([^"]+)",\s*0x([0-9a-fA-F]+),\s*\[\]byte\{([^}]*)\}\}', body)}


def witness_problems(header: str, go: str, addresses: str) -> list[str]:
    """Every witness the driver checks at boot is a row of the Go test that reads the pinned echovr.exe, with the same
    RVA and bytes, so a byte of the header that is not the game's fails a test; and the Go rows of the addresses the
    driver reads are the addresses.h values."""
    witnesses = header_witnesses(header)
    if isinstance(witnesses, str):
        return [witnesses]
    if len(witnesses) != 7:
        return [f"expected the 7 witnesses, read {len(witnesses)}"]
    rows = go_sites(go)
    problems = []
    for name, (rva, data) in witnesses.items():
        if name not in rows:
            problems.append(f"witness {name} is not a row of TestServerBotsSites")
        elif rows[name] != (rva, data):
            problems.append(f"witness {name} differs from its TestServerBotsSites row (RVA or bytes)")
    code = strip_comments(addresses)
    for name, row in (("REQUEST_ADD_BOT", "RequestAddBot"), ("FIND_POOL", "FindPool"), ("NO_SPAWN_POSITION", "no spawn position")):
        m = re.search(rf"constexpr\s+uintptr_t\s+{name}\s*=\s*0x([0-9a-fA-F]+)\s*;", code)
        if not m or row not in rows or rows[row][0] != int(m.group(1), 16):
            problems.append(f"{name} is not the RVA of the {row} row of TestServerBotsSites")
    return problems


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class ServerBotsWiring(unittest.TestCase):
    def test_the_tree(self) -> None:
        self.assertEqual(boot_problems(read(BOOT)), [])
        self.assertEqual(tick_problems(read(TICK)), [])
        self.assertEqual(callbacks_problems(read(CALLBACKS)), [])
        self.assertEqual(driver_problems(read(DRIVER)), [])
        self.assertEqual(address_problems(read(ADDRESSES)), [])
        self.assertEqual(witness_problems(read(HEADER), read(GO_SITES), read(ADDRESSES)), [])

    def test_a_witness_byte_or_rva_that_is_not_the_games_is_found(self) -> None:
        header, go, addresses = read(HEADER), read(GO_SITES), read(ADDRESSES)
        breaks = [
            (header, "0x8b, 0x47, 0x1c, 0xf7, 0xd0, 0xa8, 0x01};", "0x8b, 0x47, 0x1c, 0xf7, 0xd0, 0xa8, 0x02};"),
            (header, "{\"bot body pools\", 0x1523DF,", "{\"bot body pools\", 0x1523E0,"),
            (header, "constexpr uintptr_t kFindPoolRva = 0x502D80;", "constexpr uintptr_t kFindPoolRva = 0x502D81;"),
            (header, "    {\"bot body pools\", 0x1523DF, kBotPoolsBytes, sizeof(kBotPoolsBytes)},\n", ""),
        ]
        for text, old, new in breaks:
            with self.subTest(old=old):
                self.assertEqual(text.count(old), 1)
                self.assertTrue(witness_problems(header.replace(old, new), go, addresses))
        self.assertTrue(witness_problems(header, go, addresses.replace("= 0x201D338;", "= 0x201D33C;")))
        self.assertTrue(witness_problems(header, go.replace('{"FindPool", 0x502D80,', '{"FindPool", 0x502D84,'), addresses))

    def test_a_missing_doubled_or_gated_install_is_found(self) -> None:
        boot = read(BOOT)
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, "")))
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, INSTALL_CALL + "\n  " + INSTALL_CALL)))
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, "if (g_isServer) { " + INSTALL_CALL + " }")))
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, "// " + INSTALL_CALL)))
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, "if (g_isServer) " + INSTALL_CALL)))
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, "if (!g_isServer) return;\n  " + INSTALL_CALL)))
        self.assertTrue(boot_problems(boot.replace(INSTALL_CALL, "if (!g_isServer) {\n    return;\n  }\n  " + INSTALL_CALL)))

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
            ('"nevr_bots_per_session"', '"nevr_bots"'),
            ("isServer ? NevrCfgGetFlat", "true ? NevrCfgGetFlat"),
            ("InstallLines(isServer, text,", "InstallLines(true, text,"),
            ("g_armed.store(r.armed,", "g_armed.store(1,"),
            ("  if (wanted == 0) return;\n  if (!g_firstFrameLogged)", "  if (!g_firstFrameLogged)"),
            ("  if (wanted == 0) return;\n  const uint8_t* bytes", "  const uint8_t* bytes"),
            ("for (const Line& line : r.lines) LogLine(line);", "LogLine(r.lines[0]);"),
            ("EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info", "EchoVR::LogLevel::Info : EchoVR::LogLevel::Info"),
            ("ReadNetGame(netGame)", "NetGameView{}"),
            ("gate == Gate::kPending", "false"),
            ("LogLine(PendingLine(", "(void)(PendingLine("),
            ("pools, body);", "nullptr, body);"),
            ("UseRewrite(r, g_rewrittenFrame, frame, size);", ""),
            ("g_session = session;", ""),
            ("if (netGame == nullptr) return;", ""),
            ("if (lobby == nullptr) return;", ""),
            ("if (gate != Gate::kAsk) return;", ""),
            ("Requested(g_schedule, accepted);", ""),
            ("BuildRequest(noPosition)", "BuildRequest(kZero)"),
            ("LogLine(NoBodyLine(", "(void)(NoBodyLine("),
            ("nevr_patch_addresses::NO_SPAWN_POSITION", "nevr_patch_addresses::FIND_POOL"),
            ("WithOfflineBit(HostingFlags(lobby), [&] { return requestAddBot(netGame, request.bytes) != 0; })",
             "requestAddBot(netGame, request.bytes) != 0"),
            ("if (!BodyFree(body, 2)) {", "if (false) {"),
            ("LogLine(RequestLine(accepted", "(void)(RequestLine(accepted"),
            ("LogLine(SettingsLine(", "(void)(SettingsLine("),
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
