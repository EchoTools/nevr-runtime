"""#1423: AI bots on a dedicated server are wired once. The decisions, one server update (Drive), the reads of the
game's memory, the offline bit and every log line are tested by src/runtime/tests/test_server_bots.cpp and the sites'
bytes against the game by tests/system/mode_patches_test.go (TestServerBotsSites); these are source sensors for what
neither can see: the one Install call in boot.cpp, after the config is read and before the plugins load, the one
OnServerUpdate call in GameServerLib::Update (a server, the game-loop thread, after the shutdown hand-off) and none in
the per-frame dispatcher, the one session-start rewrite before the forward to the game and in its block, that each of
server_bots.cpp's functions is its statements exactly (it hands Drive the game's image, functions, clock, session and
thread, logs what the tested pieces give, and takes the session's lock), the three addresses, and that each site the driver checks at boot is, byte for byte, a row of the Go test that
reads the pinned echovr.exe. Each check takes the text it reads, so a test can hand it a broken copy and see it
fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "src/runtime/lifecycle/boot.cpp"
TICK = ROOT / "src/runtime/frame/tick.cpp"
GAMESERVER = ROOT / "src/runtime/server/gameserver.cpp"
CALLBACKS = ROOT / "src/runtime/server/gameserver_callbacks.cpp"
DRIVER = ROOT / "src/runtime/patch/server_bots.cpp"
ADDRESSES = ROOT / "src/runtime/hook/addresses.h"
HEADER = ROOT / "src/runtime/patch/server_bots.h"
GO_SITES = ROOT / "tests/system/mode_patches_test.go"

INSTALL_CALL = "nevr_server_bots::Install(g_isServer != FALSE);"
UPDATE_CALL = "nevr_server_bots::OnServerUpdate();"
UPDATE_GATE = "if (nevr_server_bots::DriverRuns(g_isServer != FALSE, GameLoopThreadId(), GetCurrentThreadId())) {"
HANDOFF = "if (m_gameThreadHandoff.Service()) return;"
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
    body = code.find("void RunDeferredRuntimeBootstrap(")
    if MODE_RETURN.search(code[body:at]):
        return ["RunDeferredRuntimeBootstrap returns on the mode before the Install call"]
    if "NevrCfgLogBuiltinDefaultsTaken();" not in code[body:at]:
        return ["the Install call is not after the first config.yaml read (NevrCfgLogBuiltinDefaultsTaken)"]
    if "LoadPlugins();" not in code[at:]:
        return ["the Install call is not before LoadPlugins()"]
    return []


def update_problems(gameserver: str, tick: str) -> list[str]:
    """One call, in GameServerLib::Update after the shutdown hand-off, on a server and the game-loop thread only, as
    the first statement of the gate's block; and none from the per-frame dispatcher, whose clock-read context the
    driver must not run in."""
    code = strip_comments(gameserver)
    if code.count(UPDATE_CALL) != 1:
        return [f"expected exactly one `{UPDATE_CALL}` in gameserver.cpp, found {code.count(UPDATE_CALL)}"]
    at = code.index(UPDATE_CALL)
    if depth_in(code, "VOID GameServerLib::Update()", at) != 2:
        return ["the OnServerUpdate call is not in GameServerLib::Update, in the gate's block"]
    gate = code.rfind(UPDATE_GATE, 0, at)
    if gate < 0 or code[gate + len(UPDATE_GATE):at].strip():
        return ["OnServerUpdate is not gated on a server and the game-loop thread (it calls into the game's net game)"]
    handoff = code.find(HANDOFF, code.find("VOID GameServerLib::Update()"))
    if handoff < 0 or handoff > gate:
        return ["OnServerUpdate is not after the shutdown hand-off (a server that is exiting would still add bots)"]
    if re.search(r"\breturn\b", code[handoff + len(HANDOFF):gate]):
        return ["a return between the shutdown hand-off and the bots gate can skip the driver on a running server"]
    if "nevr_server_bots::" in strip_comments(tick):
        return ["tick.cpp calls the bots driver: it runs from GameServerLib::Update only"]
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
    if not code[:at].rstrip().endswith((";", "{", "}")) or re.search(r"[{}]", code[at + len(SETTINGS_CALL):fwd]):
        return ["the rewrite is the body of an `if`, a loop or a block of its own: a frame can reach the game without it"]
    return []


# server_bots.cpp's functions, each its statements exactly, in order (whitespace aside): a statement put under an `if`,
# a statement added, dropped or changed is a problem. DriverRuns gates on the mode and the thread only: an unarmed
# server reaches OnServerUpdate and every session start, so each stops first when no bots are armed.
DRIVER_BODIES = {
    "SessionView CurrentSession()": [
        ("std::lock_guard<std::mutex> lock(g_sessionMutex);", "the session is read without its lock"),
        ("return g_session;", "CurrentSession does not return the session"),
    ],
    "void LogLine(const Line& line)": [
        ('Log(line.warning ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info, "%s", line.text.c_str());',
         "a warning line is not logged at Warning, or an Info line not at Info"),
    ],
    "void Install(bool isServer)": [
        ('const char* text = isServer ? NevrCfgGetFlat("nevr_bots_per_session") : nullptr;',
         "the count is not read from nevr_bots_per_session on a server only"),
        ("const InstallResult r = InstallLines(isServer, text, reinterpret_cast<const uint8_t*>(EchoVR::g_GameBaseAddress));",
         "Install does not decide through InstallLines on the game image"),
        ("g_armed.store(r.armed, std::memory_order_release);", "the driver is not armed with InstallLines' count alone"),
        ("for (const Line& line : r.lines) LogLine(line);", "Install does not log every line InstallLines gives"),
    ],
    "void OnServerUpdate()": [
        ("const uint32_t wanted = ArmedBots();", "OnServerUpdate does not read the armed count"),
        ("if (wanted == 0) return;", "OnServerUpdate does not stop first when no bots are armed"),
        ("uint8_t* const base = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress);",
         "the game's functions and data are not taken from the game image's base"),
        ("const auto findPool = reinterpret_cast<FindPoolFn>(base + nevr_patch_addresses::FIND_POOL);",
         "FindPool is not the game's"),
        ("const auto requestAddBot = reinterpret_cast<RequestAddBotFn>(base + nevr_patch_addresses::REQUEST_ADD_BOT);",
         "RequestAddBot is not the game's"),
        ("const std::vector<Line> lines = Drive(g_driver, wanted, nevr_return_to_lobby_queue::NetGameFromContext(g_pGame), "
         "base + nevr_patch_addresses::NO_SPAWN_POSITION, GetTickCount64(), CurrentSession(), "
         "static_cast<unsigned long>(GetCurrentThreadId()), findPool, requestAddBot);",
         "Drive is not handed the net game, the game's no-position, the clock, the session, the thread and the game's "
         "functions"),
        ("for (const Line& line : lines) LogLine(line);", "a line Drive gives is not logged"),
    ],
    "void SessionSettings(void** frame, uint64_t* size)": [
        ("const uint32_t wanted = ArmedBots();", "SessionSettings does not read the armed count"),
        ("if (wanted == 0) return;", "an unarmed server's session start is rewritten"),
        ("const uint8_t* bytes = static_cast<const uint8_t*>(*frame);", "the frame's bytes are not the session start's"),
        ("SettingsRewrite r = RewriteSessionSettings(bytes, static_cast<size_t>(*size), wanted, kPlayersPerTeam);",
         "the settings are not rewritten for the armed count"),
        ("const SessionView session{SessionIdOf(bytes, *size), BotsThisSession(r.result)};",
         "the session is not the session start's id and whether its settings make it a bot session"),
        ("{ std::lock_guard<std::mutex> lock(g_sessionMutex); g_session = session; }",
         "the session is written without its lock"),
        ("UseRewrite(r, g_rewrittenFrame, frame, size);", "the rewritten session start is not always the frame the game gets"),
        ("LogLine(SettingsLine(r, session.id, *size));", "the session-settings result is not always said"),
    ],
}


def squeeze(text: str) -> str:
    """The text with whitespace dropped except a single space between two word characters."""
    return re.sub(r"\s+(?!\w)|(?<!\w)\s+", "", re.sub(r"\s+", " ", text)).strip()


def body_of(code: str, signature: str) -> str | None:
    """The text between the braces of the function `signature` defines, or None when it has no definition."""
    m = re.search(re.escape(signature).replace(r"\ ", r"\s*") + r"\s*\{", code)
    if m is None:
        return None
    depth, at = 1, m.end()
    for k in range(m.end(), len(code)):
        depth += code[k] == "{"
        depth -= code[k] == "}"
        if depth == 0:
            return code[at:k]
    return None


def statements(body: str) -> list[str]:
    """The body's top-level statements, each squeezed: a `;` outside brackets ends one, and a block that starts a
    statement is one statement up to its closing brace."""
    out, start, depth = [], 0, 0
    for k, ch in enumerate(body):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if ch == "}" and depth == 0 and body[start:k].lstrip().startswith("{"):
                out.append(squeeze(body[start:k + 1]))
                start = k + 1
        elif ch == ";" and depth == 0:
            out.append(squeeze(body[start:k + 1]))
            start = k + 1
    if body[start:].strip():
        out.append(squeeze(body[start:]))
    return out


def driver_problems(driver: str) -> list[str]:
    """server_bots.cpp only: the decisions and the update are in server_bots.h and its GTests; this is what calls them."""
    code = strip_comments(driver)
    problems = []
    for signature, rows in DRIVER_BODIES.items():
        body = body_of(code, signature)
        if body is None:
            problems.append(f"`{signature}` is not defined in server_bots.cpp")
            continue
        found = statements(body)
        wanted = [squeeze(statement) for statement, _ in rows]
        problems += [why for (_, why), statement in zip(rows, wanted) if statement not in found]
        problems += [f"`{signature}` has a statement the wiring does not know: `{extra}`"
                     for extra in found if extra not in wanted]
        if not problems and found != wanted:
            problems.append(f"`{signature}`'s statements are out of order")
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
        self.assertEqual(update_problems(read(GAMESERVER), read(TICK)), [])
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

    def test_an_ungated_or_moved_update_call_is_found(self) -> None:
        gameserver, tick = read(GAMESERVER), read(TICK)
        call = "    " + UPDATE_CALL + "\n"
        self.assertTrue(update_problems(gameserver.replace(UPDATE_CALL, ""), tick))
        self.assertTrue(update_problems(gameserver.replace(call, call + call), tick), "a doubled call")
        self.assertTrue(update_problems(gameserver.replace(UPDATE_GATE, UPDATE_GATE + "\n    if (m_reconnectPending) return;"),
                                        tick), "a statement between the gate and the call")
        self.assertTrue(update_problems(gameserver.replace(HANDOFF, HANDOFF + "\n  if (m_reconnectPending) return;"),
                                        tick), "a return between the hand-off and the gate")
        self.assertTrue(update_problems(gameserver.replace(UPDATE_GATE, "if (true) {"), tick))
        self.assertTrue(update_problems(gameserver.replace("DriverRuns(g_isServer != FALSE,", "DriverRuns(true,"), tick))
        self.assertTrue(update_problems(gameserver.replace("GameLoopThreadId(), GetCurrentThreadId()))", "1, 1))"), tick))
        moved = gameserver.replace(call, "").replace("  nevr_return_to_lobby::Poll();\n",
                                                     "  nevr_return_to_lobby::Poll();\n" + UPDATE_GATE + "\n" + call + "  }\n")
        self.assertTrue(update_problems(moved, tick), "before the shutdown hand-off")
        self.assertTrue(update_problems(gameserver.replace(call, "") + "\nvoid Other() {\n" + UPDATE_GATE + "\n" + call + "}\n}\n", tick))
        self.assertTrue(update_problems(gameserver, tick + "\nvoid X() { nevr_server_bots::OnServerUpdate(); }\n"))
        telemetry = "  if (m_telemetry && m_telemetry->IsActive()) {\n"
        gated = "  " + UPDATE_GATE + "\n" + call + "  }\n"
        self.assertEqual(gameserver.count(gated), 1)
        deeper = gameserver.replace(gated, "").replace(telemetry, telemetry + "  " + gated.replace("\n", "\n  ", 2), 1)
        self.assertEqual(update_problems(deeper, tick),
                         ["the OnServerUpdate call is not in GameServerLib::Update, in the gate's block"], "a block deeper")

    def test_an_install_before_the_config_or_after_the_plugins_is_found(self) -> None:
        boot = read(BOOT)
        without = boot.replace(INSTALL_CALL, "")
        config = without.index("NevrCfgLogBuiltinDefaultsTaken();")
        self.assertTrue(boot_problems(without[:config] + INSTALL_CALL + "\n    " + without[config:]))
        trigger = without.index(";", without.index("runtime bootstrap trigger=%s")) + 1
        self.assertTrue(boot_problems(without[:trigger] + "\n  " + INSTALL_CALL + without[trigger:]))
        plugins = without.index("LoadPlugins();") + len("LoadPlugins();")
        self.assertTrue(boot_problems(without[:plugins] + "\n  " + INSTALL_CALL + without[plugins:]))

    def test_a_rewrite_after_the_forward_or_elsewhere_is_found(self) -> None:
        callbacks = read(CALLBACKS)
        self.assertTrue(callbacks_problems(callbacks.replace(SETTINGS_CALL, "")))
        moved = callbacks.replace(SETTINGS_CALL, "").replace(
            '"SNSLobbyStartSessionv4", data, size);', '"SNSLobbyStartSessionv4", data, size);\n' + SETTINGS_CALL, 1)
        self.assertTrue(callbacks_problems(moved))
        for gated in ("if (size == 0) " + SETTINGS_CALL, "if (size == 0) { " + SETTINGS_CALL + " }", "{ " + SETTINGS_CALL + " }"):
            with self.subTest(gated=gated):
                self.assertTrue(callbacks_problems(callbacks.replace(SETTINGS_CALL, gated)), gated)

    def test_a_broken_driver_is_found(self) -> None:
        driver = read(DRIVER)
        breaks = [
            ('"nevr_bots_per_session"', '"nevr_bots"'),
            ("isServer ? NevrCfgGetFlat", "true ? NevrCfgGetFlat"),
            ("InstallLines(isServer, text,", "InstallLines(true, text,"),
            ("reinterpret_cast<const uint8_t*>(EchoVR::g_GameBaseAddress))", "reinterpret_cast<const uint8_t*>(nullptr))"),
            ("g_armed.store(r.armed,", "g_armed.store(1,"),
            ("  if (wanted == 0) return;\n  uint8_t* const base", "  uint8_t* const base"),
            ("  if (wanted == 0) return;\n  const uint8_t* bytes", "  const uint8_t* bytes"),
            ("for (const Line& line : r.lines) LogLine(line);", "LogLine(r.lines[0]);"),
            ("EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info", "EchoVR::LogLevel::Info : EchoVR::LogLevel::Info"),
            ("reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress);", "reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress) + 0x1000;"),
            ("base + nevr_patch_addresses::FIND_POOL)", "base + nevr_patch_addresses::REQUEST_ADD_BOT)"),
            ("base + nevr_patch_addresses::REQUEST_ADD_BOT)", "base + nevr_patch_addresses::FIND_POOL)"),
            ("NetGameFromContext(g_pGame)", "NetGameFromContext(nullptr)"),
            ("base + nevr_patch_addresses::NO_SPAWN_POSITION,", "base + nevr_patch_addresses::FIND_POOL,"),
            ("GetTickCount64(), CurrentSession()", "0, CurrentSession()"),
            ("CurrentSession(),\n", "SessionView{},\n"),
            ("static_cast<unsigned long>(GetCurrentThreadId())", "static_cast<unsigned long>(0)"),
            ("findPool, requestAddBot);", "requestAddBot, findPool);"),
            ("for (const Line& line : lines) LogLine(line);", ""),
            ("  std::lock_guard<std::mutex> lock(g_sessionMutex);\n  return g_session;", "  return g_session;"),
            ("    std::lock_guard<std::mutex> lock(g_sessionMutex);\n    g_session = session;", "    g_session = session;"),
            ("SessionIdOf(bytes, *size)", "std::string(\"none\")"),
            ("BotsThisSession(r.result)", "true"),
            ("UseRewrite(r, g_rewrittenFrame, frame, size);", ""),
            ("LogLine(SettingsLine(", "(void)(SettingsLine("),
            ("wanted, kPlayersPerTeam)", "wanted, 4)"),
            ("  UseRewrite(r, g_rewrittenFrame, frame, size);", "  if (*size == 0) UseRewrite(r, g_rewrittenFrame, frame, size);"),
            ("  UseRewrite(r, g_rewrittenFrame, frame, size);", "  if (*size == 0) {\n    UseRewrite(r, g_rewrittenFrame, frame, size);\n  }"),
            ("  g_armed.store(r.armed, std::memory_order_release);\n",
             "  g_armed.store(r.armed, std::memory_order_release);\n  if (r.armed == 0 && isServer) g_armed.store(1, std::memory_order_release);\n"),
            ("g_armed.store(r.armed,", "g_armed.store(r.armed ? r.armed : 1,"),
            ("  LogLine(SettingsLine(", "  if (r.result != SettingsResult::kNotAMatch) LogLine(SettingsLine("),
            ("  const uint32_t wanted = ArmedBots();\n  if (wanted == 0) return;\n  uint8_t* const base",
             "  const uint32_t wanted = ArmedBots();\n  uint8_t* const base"),
            ("  UseRewrite(r, g_rewrittenFrame, frame, size);\n  LogLine(SettingsLine(r, session.id, *size));",
             "  LogLine(SettingsLine(r, session.id, *size));\n  UseRewrite(r, g_rewrittenFrame, frame, size);"),
            ("void OnServerUpdate() {", "void OnServerUpdateOld() {"),
            ("  for (const Line& line : r.lines) LogLine(line);\n}",
             "  for (const Line& line : r.lines) LogLine(line);\n  if (r.armed == 0) {\n    g_armed.store(1, std::memory_order_release);\n  }\n}"),
        ]
        for old, new in breaks:
            with self.subTest(old=old):
                self.assertEqual(driver.count(old), 1, old)
                self.assertTrue(driver_problems(driver.replace(old, new)))
        self.assertEqual(driver_problems(driver.replace("  UseRewrite(r, g_rewrittenFrame, frame, size);\n", "")),
                         ["the rewritten session start is not always the frame the game gets"], "a statement dropped")
        self.assertEqual(driver_problems(driver.replace("  for (const Line& line : r.lines) LogLine(line);\n}",
                                                        "  for (const Line& line : r.lines) LogLine(line);\n  Arm();\n}")),
                         ["`void Install(bool isServer)` has a statement the wiring does not know: `Arm();`"],
                         "a statement added")
        for added in ("\nvoid X(uint32_t* hostingFlags) { *hostingFlags = 1; }\n",
                      "\nvoid Y(uint32_t* f) { *f |= kHostingFlagOffline; }\n"):
            with self.subTest(added=added):
                self.assertTrue(driver_problems(driver + added), "a driver that writes the hosting flags itself")

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
