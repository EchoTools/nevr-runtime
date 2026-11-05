"""#2109: a dedicated server ticks at the idle rate only while it runs no session, and a session never runs at it. The
decisions are pure and tested by src/runtime/tests/test_server_timestep.cpp (IdleTickRate, SessionEndIdle,
SessionStepFallback, UntypedSessionStart); these are source sensors for the wiring neither can see:
- the boot write is the idle rate, and a session writes its rate through ApplySessionStep (the fallback that never
  leaves a session at the idle step);
- only the game's own session end (IServerLib::EndSession, kGameEnded) and the service's CODE_ENDED for the current
  session idle the server: a ServerDB reconnect, a late CODE_ENDED and a shutdown do not, and the lobby-settings handler,
  which every context EndSession reaches, does not;
- a legacy start frame with no game type writes a session rate over the idle step.
Each check takes the text it reads, so a test hands it a broken copy and sees it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "src/runtime/lifecycle/config.cpp"
CALLBACKS = ROOT / "src/runtime/server/gameserver_callbacks.cpp"
GAMESERVER = ROOT / "src/runtime/server/gameserver.cpp"
SERVERDB = ROOT / "src/runtime/server/gameserver_serverdb.cpp"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def function_body(code: str, head: str) -> str | None:
    """The braced body of the first function whose text starts with `head`, or None."""
    start = code.find(head)
    if start < 0:
        return None
    open_at = code.find("{", start)
    if open_at < 0:
        return None
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[open_at + 1:i]
    return None


def timestep_problems(config: str, callbacks: str) -> list[str]:
    problems = []
    code = strip_comments(config)
    boot = function_body(code, "static void ApplyServerTimestep(")
    if boot is None or "const uint32_t bootRate = nevr_server_timestep::kIdleTickRateHz;" not in boot:
        problems.append("ApplyServerTimestep does not write the idle rate at boot")
    elif "ApplyStep(g_isServer != FALSE, bootRate, stepField)" not in boot:
        problems.append("ApplyServerTimestep does not write bootRate")
    session = function_body(code, "void ApplySessionTimestep(")
    if session is None or "nevr_server_timestep::ApplySessionStep(" not in session:
        problems.append("ApplySessionTimestep does not write through ApplySessionStep (RateToWrite, then the fallback "
                        "that never leaves a session at the idle step)")
    idle = function_body(code, "void ApplyServerIdleTimestep(")
    if idle is None or "nevr_server_timestep::kIdleTickRateHz" not in idle:
        problems.append("ApplyServerIdleTimestep does not write the idle rate")
    return problems


def session_end_problems(callbacks: str, gameserver: str, serverdb: str) -> list[str]:
    """Only the game's own end and the service's CODE_ENDED for the current session put the server on the idle step."""
    problems = []
    cb = strip_comments(callbacks)
    handler = function_body(cb, "m_context->SetLobbySettingsClearedHandler(")
    if handler is None:
        problems.append("the lobby-settings-cleared handler is not found in gameserver_callbacks.cpp")
    elif "ApplyServerIdleTimestep" in handler:
        problems.append("the lobby-settings-cleared handler idles the server: every context EndSession reaches it, "
                        "a reconnect's included")
    reconnect = function_body(cb, "void GameServerLib::ProcessPendingReconnect(")
    if reconnect is None:
        problems.append("ProcessPendingReconnect is not found")
    elif "ApplyServerIdleTimestep" in reconnect or "EndServerSession" in reconnect:
        problems.append("ProcessPendingReconnect idles the server: the game's match goes on through a reconnect")
    ended = re.search(r"CODE_ENDED\)\s*\{(.*?)\n\s*break;", cb, flags=re.S)
    if ended is None:
        problems.append("the CODE_ENDED case is not found")
    else:
        body = ended.group(1)
        decide = body.find("nevr_server_timestep::IdlesAfterSessionEnd(")
        current = body.find("GetSessionState().lobbySessionId")
        end = body.find("GetContext().EndSession()")
        if decide < 0 or "SessionEndCause::kServiceEnded" not in body or "event.lobby_session_id()" not in body[decide:]:
            problems.append("CODE_ENDED does not decide the idle step with IdlesAfterSessionEnd(kServiceEnded, the "
                            "event's id, the current id)")
        if current < 0 or end < 0 or current > end:
            problems.append("CODE_ENDED does not read the current session id before EndSession clears it")
        idle = body.find("ApplyServerIdleTimestep(")
        if idle >= 0 and (decide < 0 or idle < decide):
            problems.append("CODE_ENDED idles the server before IdlesAfterSessionEnd decides")
    gs = strip_comments(gameserver)
    entry = function_body(gs, "VOID GameServerLib::EndSession()")
    if entry is None or "EndServerSession(nevr_server_timestep::SessionEndCause::kGameEnded)" not in entry:
        problems.append("IServerLib::EndSession (the game's own end) does not end the session as kGameEnded")
    body = function_body(gs, "void GameServerLib::EndServerSession(")
    if body is None:
        problems.append("EndServerSession is not found in gameserver.cpp")
    else:
        decide = body.find("nevr_server_timestep::IdlesAfterSessionEnd(cause")
        idle = body.find("ApplyServerIdleTimestep(")
        if decide < 0 or idle < 0 or idle < decide:
            problems.append("EndServerSession does not idle the server through IdlesAfterSessionEnd(cause, ...)")
    sd = strip_comments(serverdb)
    for head in ("void GameServerLib::ShutdownUnregisterOnGameThread(",
                 "void GameServerLib::ShutdownUnregisterOffGameThread("):
        b = function_body(sd, head)
        if b is None or "EndServerSession(nevr_server_timestep::SessionEndCause::kShutdown)" not in b:
            problems.append(f"{head.split('::')[1].rstrip('(')} does not end the session as kShutdown")
        elif re.search(r"(?<![\w:])EndSession\(\)", b):
            problems.append(f"{head.split('::')[1].rstrip('(')} still calls EndSession(), which idles the server")
    return problems


def untyped_start_problems(callbacks: str, config: str) -> list[str]:
    """A legacy start frame with no game type writes a session rate over the idle step."""
    problems = []
    cb = strip_comments(callbacks)
    start = cb.find("nevr_game_type_table::GameTypeOfSessionStart(")
    if start < 0:
        return ["the legacy session start's game-type read is not found"]
    window = cb[start:start + 900]
    if not re.search(r"\}\s*else\s*\{\s*ApplyUntypedSessionTimestep\(", window):
        problems.append("a legacy start frame with no game type writes no session rate (the server would run the "
                        "session at the idle step)")
    body = function_body(strip_comments(config), "void ApplyUntypedSessionTimestep(")
    if body is None or "nevr_server_timestep::UntypedSessionStartWrites(" not in body:
        problems.append("ApplyUntypedSessionTimestep does not decide with UntypedSessionStartWrites")
    elif "ApplySessionTimestep(" not in body:
        problems.append("ApplyUntypedSessionTimestep does not write through ApplySessionTimestep")
    return problems


class ServerIdleTickWiring(unittest.TestCase):
    def test_the_boot_writes_the_idle_step_and_a_session_its_rate(self):
        self.assertEqual(timestep_problems(CONFIG.read_text(), CALLBACKS.read_text()), [])

    def test_a_boot_that_writes_the_session_rate_is_caught(self):
        broken = CONFIG.read_text().replace("const uint32_t bootRate = nevr_server_timestep::kIdleTickRateHz;",
                                            "const uint32_t bootRate = g_headlessTickRateHz;")
        self.assertTrue(timestep_problems(broken, CALLBACKS.read_text()))

    def test_a_session_write_without_the_fallback_is_caught(self):
        broken = CONFIG.read_text().replace("nevr_server_timestep::ApplySessionStep(",
                                            "nevr_server_timestep::ApplyStepNoFallback(")
        self.assertTrue(timestep_problems(broken, CALLBACKS.read_text()))

    def test_only_a_real_end_of_the_current_session_idles(self):
        self.assertEqual(session_end_problems(CALLBACKS.read_text(), GAMESERVER.read_text(), SERVERDB.read_text()), [])

    def test_a_handler_that_idles_on_every_context_end_is_caught(self):
        # The shape of 083299f7: the lobby-settings handler idled at "session_end", which a reconnect reaches.
        broken = CALLBACKS.read_text().replace(
            "m_context->SetLobbySettingsClearedHandler(\n      [](const nevr_game_server::LobbySettingsRecord& cleared, "
            "const char* reason, const std::string& sessionId) {\n",
            "m_context->SetLobbySettingsClearedHandler(\n      [](const nevr_game_server::LobbySettingsRecord& cleared, "
            "const char* reason, const std::string& sessionId) {\n        ApplyServerIdleTimestep(reason, sessionId);\n")
        self.assertNotEqual(broken, CALLBACKS.read_text())
        self.assertTrue(session_end_problems(broken, GAMESERVER.read_text(), SERVERDB.read_text()))

    def test_a_reconnect_that_idles_is_caught(self):
        broken = CALLBACKS.read_text().replace(
            "(IdlesAfterSessionEnd: kReconnect).\n  m_context->EndSession();\n",
            "(IdlesAfterSessionEnd: kReconnect).\n  m_context->EndSession();\n  ApplyServerIdleTimestep(\"reconnect\", \"\");\n")
        self.assertNotEqual(broken, CALLBACKS.read_text())
        self.assertTrue(session_end_problems(broken, GAMESERVER.read_text(), SERVERDB.read_text()))

    def test_a_code_ended_that_idles_whatever_session_it_names_is_caught(self):
        text = CALLBACKS.read_text()
        broken = text.replace("nevr_server_timestep::IdlesAfterSessionEnd(", "AlwaysIdle(", 1)
        self.assertNotEqual(broken, text)
        self.assertTrue(session_end_problems(broken, GAMESERVER.read_text(), SERVERDB.read_text()))

    def test_a_shutdown_that_idles_is_caught(self):
        text = SERVERDB.read_text()
        broken = text.replace("EndServerSession(nevr_server_timestep::SessionEndCause::kShutdown);", "EndSession();", 1)
        self.assertNotEqual(broken, text)
        self.assertTrue(session_end_problems(CALLBACKS.read_text(), GAMESERVER.read_text(), broken))

    def test_the_games_own_end_idles(self):
        text = GAMESERVER.read_text()
        broken = text.replace("EndServerSession(nevr_server_timestep::SessionEndCause::kGameEnded);",
                              "EndServerSession(nevr_server_timestep::SessionEndCause::kShutdown);")
        self.assertNotEqual(broken, text)
        self.assertTrue(session_end_problems(CALLBACKS.read_text(), broken, SERVERDB.read_text()))

    def test_an_untyped_start_writes_a_session_rate(self):
        self.assertEqual(untyped_start_problems(CALLBACKS.read_text(), CONFIG.read_text()), [])
        text = CALLBACKS.read_text()
        broken = text.replace("        ApplyUntypedSessionTimestep(nevr_server_bots::SessionIdOf(",
                              "        (void)(nevr_server_bots::SessionIdOf(", 1)
        self.assertNotEqual(broken, text)
        self.assertTrue(untyped_start_problems(broken, CONFIG.read_text()))


if __name__ == "__main__":
    unittest.main()
