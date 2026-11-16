# ADR 0044 recorded reason: the wiring is C++ call-site order in files no test target links, checked over comment-stripped source
# (the same form as test_join_hold_wiring.py), until the unit of #2132 ports it.
"""PLAN-v5 R6-a-2b: the server's session-start hold is wired where its rules need it.

src/runtime/tests/test_session_demand_rules.cpp drives the pure rules (server/session_demand_rules.h). The call sites live in
gameserver_callbacks.cpp, gameserver.cpp and boot.cpp, which no test target links, so their order is checked here:

  - the legacy session start is held (HoldSessionStart) before it is forwarded, and the forward is the only send of
    LobbyStartSessionV4, inside ForwardLegacySessionStart;
  - an accept that arrives while a start is held is queued (HoldAccept) before the broadcaster send of
    LobbyAcceptPlayersSuccessV2, in the one send site;
  - PollSessionDemand runs between ProcessReceivedMessages and ReleaseDue in GameServerLib::Update;
  - the release branch of PollSessionDemand forwards the held start (ReleaseHeldSessionStart);
  - the one send of an accept to the game (SendEntrantsAcceptToGame) has exactly two callers, the accept case of
    OnTcpMsgProtobuf and ReleaseHeldSessionStart, one call each, and each call is gated by nevr_join_hold::Admit first;
  - a session's end drops a held start only when it names the held session (EndsHeldSession, F1), before any state is cleared;
  - the game's own end (kGameEnded) does not call the hold's end at all (F1, PLAN-v5 R6-a-2b review); only kShutdown does, in
    EndServerSession, and the service's CODE_ENDED path calls it with the id the service named;
  - a second start that replaces a held one is logged (session demand replaced) before the held state is overwritten (N3);
  - a reconnect keeps a held start: ProcessPendingReconnect does not touch the hold;
  - a failed resolve ends the session through EndServerSession(kContentFailed), and nothing in session_demand.cpp stops the
    process (ServerFatal);
  - Shutdown cancels the worker before m_shutdownComplete is set;
  - a server installs the read hook (EnsureResolverHook) in boot.cpp, and a failed install is ServerFatal;
  - the build lists the source and the test target.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import strip_comments


ROOT = Path(__file__).resolve().parents[2]


def code(path: str) -> str:
    return strip_comments((ROOT / path).read_text())


def between(text: str, start: str, end: str) -> str:
    a = text.index(start)
    return text[a:text.index(end, a)]


FUNCTION_HEADER = re.compile(
    r"^(?:[A-Za-z_][^\n]*\("  # a column-0 line that opens a call or a definition
    r"|[ \t]+(?:static\s+|inline\s+)*(?!(?:if|for|while|switch|catch|return)\b)[A-Za-z_][\w:<>\*&]*[ \t\*&]+\w+\([^;\n]*\)\s*(?:const\s*)?\{)",
    re.M)  # an indented definition with its body open on the same line (a helper inside a namespace or a class)


def send_sites(text: str) -> list[tuple[str, bool]]:
    """(caller, gated) for each call of SendEntrantsAcceptToGame, in file order. The definition is not a call (it is preceded by
    `void `). The caller is the nearest line that opens a function before the call (a column-0 header, or an indented definition
    with its `{` on the line, so a helper defined inside a namespace is not named after the function above it); gated is true when
    nevr_join_hold::Admit( appears in that caller before the call."""
    headers = [(m.start(), re.search(r"([A-Za-z_]\w*)\s*\(", m.group(0)).group(1))
               for m in FUNCTION_HEADER.finditer(text)]
    sites = []
    for call in re.finditer(r"(?<!void )SendEntrantsAcceptToGame\(", text):
        start, name = max((h for h in headers if h[0] <= call.start()), key=lambda h: h[0])
        sites.append((name, "nevr_join_hold::Admit(" in text[start:call.start()]))
    return sites


class SessionDemandWiring(unittest.TestCase):
    def test_legacy_start_is_held_before_it_is_forwarded(self):
        cb = code("src/runtime/server/gameserver_callbacks.cpp")
        handler = between(cb, "msgId == SYM_LEGACY_SESSION_START", "msgId == SYM_LEGACY_PLAYERS_REJECTED")
        self.assertIn("nevr_session_demand::HoldSessionStart(", handler)
        self.assertLess(handler.index("HoldSessionStart("), handler.index("ForwardLegacySessionStart("))

    def test_start_send_lives_only_in_the_forward(self):
        cb = code("src/runtime/server/gameserver_callbacks.cpp")
        forward = between(cb, "static void ForwardLegacySessionStart(", "void ReleaseHeldSessionStart(")
        self.assertIn("Sym::LobbyStartSessionV4", forward)
        self.assertEqual(cb.count("Sym::LobbyStartSessionV4"), 1)

    def test_accept_is_queued_before_its_send(self):
        cb = code("src/runtime/server/gameserver_callbacks.cpp")
        site = between(cb, "case gameservice::v1::Envelope::kLobbyEntrantsAccept", "failed to encode entrants accept")
        self.assertIn("nevr_session_demand::HoldAccept(", site)
        self.assertLess(site.index("nevr_session_demand::HoldAccept("), site.index("SendEntrantsAcceptToGame("))

    def test_poll_runs_between_the_message_pump_and_the_release(self):
        gs = code("src/runtime/server/gameserver.cpp")
        update = between(gs, "ProcessReceivedMessages();", "nevr_join_hold::ReleaseDue(")
        self.assertIn("PollSessionDemand();", update)

    def test_failed_resolve_ends_the_session_not_the_process(self):
        gs = code("src/runtime/server/gameserver.cpp")
        poll = between(gs, "void GameServerLib::PollSessionDemand()", "VOID GameServerLib::EndSession()")
        self.assertIn("EndServerSession(nevr_server_timestep::SessionEndCause::kContentFailed)", poll)
        demand = code("src/runtime/server/session_demand.cpp")
        self.assertNotIn("ServerFatal", demand)
        self.assertNotIn("ForceFatalExit", demand)

    def test_shutdown_cancels_the_worker_before_the_flag(self):
        gs = code("src/runtime/server/gameserver.cpp")
        self.assertIn("nevr_session_demand::Shutdown();", gs)
        self.assertLess(gs.index("nevr_session_demand::Shutdown();"), gs.index("m_shutdownComplete.store(true)"))

    def test_server_installs_the_read_hook_or_stops(self):
        boot = code("src/runtime/lifecycle/boot.cpp")
        self.assertIn("if (!g_isServer) {", boot)
        tail = between(boot, "} else if (!nevr::assets::EnsureResolverHook()) {", "CheckGameData();")
        self.assertIn("ServerFatal(", tail)

    def test_the_release_branch_forwards_the_held_start(self):
        gs = code("src/runtime/server/gameserver.cpp")
        poll = between(gs, "void GameServerLib::PollSessionDemand()", "VOID GameServerLib::EndSession()")
        branch = between(poll, "DueAction::kRelease", "DueAction::kFail")
        self.assertEqual(branch.count("ReleaseHeldSessionStart(this, std::move(due));"), 1)

    def test_an_accept_is_sent_by_exactly_its_two_callers_each_gated(self):
        sites = send_sites(code("src/runtime/server/gameserver_callbacks.cpp"))
        self.assertEqual([name for name, _ in sites], ["OnTcpMsgProtobuf", "ReleaseHeldSessionStart"])
        self.assertEqual([gated for _, gated in sites], [True, True])

    def test_a_session_end_drops_only_the_held_session(self):
        demand = code("src/runtime/server/session_demand.cpp")
        body = between(demand, "void OnSessionEnd(", "void Shutdown()")
        self.assertIn("EndsHeldSession(g_sessionId, ended)", body)
        self.assertLess(body.index("EndsHeldSession("), body.index("g_activeTicket = 0"))
        self.assertLess(body.index("EndsHeldSession("), body.index("g_hold.Clear()"))

    def test_the_game_end_does_not_call_the_hold_end(self):
        gs = code("src/runtime/server/gameserver.cpp")
        body = " ".join(between(gs, "void GameServerLib::EndServerSession(", "const auto sendEnvelope").split())
        self.assertIn("if (cause == nevr_server_timestep::SessionEndCause::kShutdown) { nevr_session_demand::OnSessionEnd(", body)
        self.assertEqual(gs.count("nevr_session_demand::OnSessionEnd("), 1)

    def test_a_second_hold_logs_what_it_replaces_before_it_overwrites(self):
        demand = code("src/runtime/server/session_demand.cpp")
        start = between(demand, "bool HoldSessionStart(", "bool Active()")
        self.assertIn("if (g_hold.Replaced()) {", start)
        self.assertIn("session demand replaced", start)
        self.assertLess(start.index("session demand replaced"), start.index("g_sessionId = sessionId"))
        self.assertLess(start.index("g_hold.Replaced()"), start.index("g_frame.assign("))

    def test_send_sites_names_an_indented_helper_by_its_own_name(self):
        # Planted: a helper defined inside a namespace, after a column-0 caller. Named after the column-0 caller, the call would be
        # misattributed (the old header rule did exactly that); the helper's own name is the truth.
        planted = (
            "void ReleaseHeldSessionStart(GameServerLib* self) {\n"
            "  nevr_join_hold::Admit(x);\n"
            "}\n"
            "namespace {\n"
            "  void Hidden(int b) {\n"
            "    SendEntrantsAcceptToGame(b);\n"
            "  }\n"
            "}  // namespace\n")
        self.assertEqual(send_sites(planted), [("Hidden", False)])

    def test_send_sites_gates_a_column_zero_caller_before_its_send(self):
        planted = (
            "void Caller(int a) {\n"
            "  if (nevr_join_hold::Admit(a)) {\n"
            "    SendEntrantsAcceptToGame(a);\n"
            "  }\n"
            "}\n"
            "void Ungated() {\n"
            "  SendEntrantsAcceptToGame(0);\n"
            "}\n")
        self.assertEqual(send_sites(planted), [("Caller", True), ("Ungated", False)])

    def test_a_reconnect_keeps_the_held_start(self):
        cb = code("src/runtime/server/gameserver_callbacks.cpp")
        reconnect = between(cb, "void GameServerLib::ProcessPendingReconnect()", "SessionState state = m_context->GetSessionState();")
        self.assertNotIn("nevr_session_demand::", reconnect)

    def test_build_lists_the_source_and_the_test(self):
        cmake = (ROOT / "src/runtime/CMakeLists.txt").read_text()
        self.assertIn('"server/session_demand.cpp"', cmake)
        self.assertIn("add_executable(test_session_demand_rules tests/test_session_demand_rules.cpp)", cmake)


if __name__ == "__main__":
    unittest.main()
