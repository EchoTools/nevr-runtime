"""#1737: the join hold on the PC dedicated server is wired where its rules need it.

src/runtime/tests/test_join_hold.cpp drives the pure gate (server/join_hold_rules.h). The call sites live in
gameserver_callbacks.cpp, gameserver.cpp and boot.cpp, which no test target links, so their order is checked here over
comment-stripped source:

  - the accept's hold (Admit) comes before the broadcaster send of LobbyAcceptPlayersSuccessV2, in the one send site;
  - the release (ReleaseDue) runs after ProcessReceivedMessages in GameServerLib::Update;
  - a session create arms it, the service's CODE_ENDED and EndServerSession clear it, a reconnect's EndSession does not;
  - Install runs after nevr_server_frame_wait::Install in the deferred bootstrap;
  - network.join_hold_ms is a known key and has no flat-key map entry (the N133 map stays 35);
  - the build lists the source and the test, and the test runs in test-auth-unit.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def code(path: str) -> str:
    return strip_comments(read(path))


def between(text: str, start: str, end: str) -> str:
    a = text.index(start)
    return text[a:text.index(end, a)]


def callbacks() -> str:
    return code("src/runtime/server/gameserver_callbacks.cpp")


def accept_case() -> str:
    return between(callbacks(), "case gameservice::v1::Envelope::kLobbyEntrantsAccept", "case gameservice::v1::Envelope::kLobbyEntrantReject")


class TheHoldSitsBeforeTheSend(unittest.TestCase):
    def test_admit_precedes_the_broadcaster_send_of_the_accept(self):
        body = accept_case()
        self.assertIn("nevr_join_hold::Admit(", body)
        self.assertLess(body.index("nevr_join_hold::Admit("), body.index("Sym::LobbyAcceptPlayersSuccessV2"))

    def test_the_accept_is_sent_in_one_place_only(self):
        self.assertEqual(callbacks().count("Sym::LobbyAcceptPlayersSuccessV2"), 1)

    def test_a_held_accept_is_not_sent_now(self):
        body = accept_case()
        self.assertRegex(body, r"if\s*\(\s*nevr_join_hold::Admit\([^;]*\)\s*\)\s*\{?\s*break;|!\s*nevr_join_hold::Admit\(")


class TheReleaseRunsOnTheGameThread(unittest.TestCase):
    def test_release_follows_process_received_messages_in_update(self):
        body = extract_braced_function(read("src/runtime/server/gameserver.cpp"), "VOID GameServerLib::Update(")
        self.assertIn("nevr_join_hold::ReleaseDue(", body)
        self.assertLess(body.index("ProcessReceivedMessages()"), body.index("nevr_join_hold::ReleaseDue("))


class TheSessionsArmAndClearIt(unittest.TestCase):
    def test_session_create_arms(self):
        body = between(callbacks(), "case gameservice::v1::Envelope::kLobbySessionCreate", "case gameservice::v1::Envelope::kLobbySessionEvent")
        self.assertIn("nevr_join_hold::OnSessionCreate(", body)

    def test_the_service_ending_the_session_clears(self):
        body = between(callbacks(), "case gameservice::v1::Envelope::kLobbySessionEvent", "case gameservice::v1::Envelope::kLobbySessionSuccessV5")
        self.assertRegex(body, r"nevr_join_hold::OnSessionEnd\(\s*\"service_ended\"")

    def test_end_server_session_clears(self):
        body = extract_braced_function(read("src/runtime/server/gameserver.cpp"), "void GameServerLib::EndServerSession(")
        self.assertRegex(body, r"nevr_join_hold::OnSessionEnd\(")

    def test_a_reconnects_end_session_does_not_clear(self):
        text = callbacks()
        start = text.index("m_context->EndSession();")
        window = text[max(0, start - 1500):start + 200]
        self.assertNotIn("nevr_join_hold", window)


class InstallRunsAfterTheFrameWait(unittest.TestCase):
    def test_install_follows_server_frame_wait_install(self):
        boot = code("src/runtime/lifecycle/boot.cpp")
        self.assertIn("nevr_join_hold::Install(g_isServer != FALSE)", boot)
        self.assertLess(boot.index("nevr_server_frame_wait::Install("), boot.index("nevr_join_hold::Install("))


class TheKeyAndTheBuild(unittest.TestCase):
    def test_join_hold_ms_is_a_known_network_key(self):
        text = read("src/core/nevr_config.cpp")
        network = between(text, '{"network",', '{"assets"')
        self.assertIn('"join_hold_ms"', network)

    def test_the_flat_key_map_has_no_join_hold_entry(self):
        self.assertNotIn("join_hold", read("src/runtime/lifecycle/service_map.cpp"))

    def test_the_getter_reads_the_key_directly(self):
        self.assertRegex(code("src/runtime/lifecycle/service_config.cpp"), r"const char\* NevrCfgJoinHoldMs\(\)")
        self.assertIn('"network.join_hold_ms"', read("src/runtime/lifecycle/service_config.cpp"))

    def test_the_source_and_the_test_are_built(self):
        cmake = read("src/runtime/CMakeLists.txt")
        self.assertIn('"server/join_hold.cpp"', cmake)
        self.assertIn("add_executable(test_join_hold tests/test_join_hold.cpp)", cmake)

    def test_the_test_runs_in_test_auth_unit(self):
        self.assertGreaterEqual(read("justfile").count("test_join_hold"), 2)

    def test_winsock_precedes_windows_h(self):
        text = read("src/runtime/server/join_hold.cpp")
        self.assertLess(text.index("#include <winsock2.h>"), text.index("#include <windows.h>"))

    def test_the_default_is_off(self):
        self.assertRegex(read("src/runtime/server/join_hold_rules.h"), r"kDefaultHoldMs\s*=\s*0;")


if __name__ == "__main__":
    unittest.main()
