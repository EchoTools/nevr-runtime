"""#905: the lobby settings relay is wired to the ServerDB socket and to the game.

Source sensors for what the GTests in src/runtime/tests/test_lobby_settings_relay.cpp and
test_server_context.cpp cannot reach (they drive the pure relay with fake sinks and the context with a fake
handler; the two game calls and the log live in gameserver_callbacks.cpp, which no test target links):

  - the ServerDB message handler routes the TCP form of the message (CSymbol64("SNSLobbySettingsResponse")) to the
    relay, ahead of the "Unhandled WebSocket msgId" branch;
  - the relay's two sinks are the game's local event and a send to all peers, both under the local-event symbol;
  - a relayed document is recorded in the context only when the relay succeeded, and the report is logged at the
    level the relay chose;
  - the context reports the cleared record from StartSession and EndSession after it has released its lock;
  - the two symbols carry the values read from echovr.exe.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def callbacks() -> str:
    return read("src/runtime/server/gameserver_callbacks.cpp")


def relay_body() -> str:
    return extract_braced_function(callbacks(), "void RelayLobbySettingsFrame(")


def tcp_callbacks_body() -> str:
    return extract_braced_function(callbacks(), "void GameServerLib::RegisterTcpCallbacks()")


class MessageHandlerRoutesTheFrame(unittest.TestCase):
    def test_the_tcp_form_reaches_the_relay(self):
        body = tcp_callbacks_body()
        self.assertRegex(
            body,
            r"msgId\s*==\s*TcpSym::LobbySettingsResponse\s*\)\s*\{\s*RelayLobbySettingsFrame\(\s*this\s*,\s*data\s*,\s*size\s*\)",
        )

    def test_it_is_routed_before_the_unhandled_branch(self):
        body = tcp_callbacks_body()
        self.assertLess(body.index("TcpSym::LobbySettingsResponse"), body.index("Unhandled WebSocket msgId"))

    def test_the_unhandled_branch_is_still_the_last_one(self):
        body = tcp_callbacks_body()
        self.assertEqual(body.count("Unhandled WebSocket msgId"), 1)
        last_branch = body.rindex("} else {")
        self.assertGreater(body.index("Unhandled WebSocket msgId"), last_branch)


class RelayUsesTheGameCalls(unittest.TestCase):
    def test_local_event_uses_the_local_symbol_and_the_matching_name(self):
        body = relay_body()
        self.assertRegex(
            body,
            r"EchoVR::BroadcasterReceiveLocalEvent\(\s*broadcaster\s*,\s*Sym::LobbySettingsResponse\s*,\s*\"SNSLobbySettingsResponse\"",
        )

    def test_send_goes_out_under_the_local_symbol(self):
        body = relay_body()
        self.assertRegex(body, r"EchoVR::BroadcasterSend\(\s*broadcaster\s*,\s*Sym::LobbySettingsResponse\s*,")

    def test_the_peer_is_the_one_the_relay_passes_not_a_literal(self):
        body = relay_body()
        send = body[body.index("EchoVR::BroadcasterSend(") :]
        send = send[: send.index(");")]
        self.assertIn("peer", send)

    def test_the_all_peers_constant_matches_the_games(self):
        code = strip_comments(callbacks())
        self.assertRegex(
            code,
            r"static_assert\(\s*nevr_game_server::kLobbySettingsAllPeers\s*==\s*EchoVR::Peer_AllPeers",
        )

    def test_the_report_is_logged_at_the_level_the_relay_chose(self):
        body = relay_body()
        self.assertIn("LobbySettingsLogLevel::kWarning", body)
        self.assertRegex(body, r"Log\(\s*level\s*,\s*\"%s\"\s*,\s*report\.line\.c_str\(\)\s*\)")

    def test_the_record_is_written_only_when_relayed(self):
        body = relay_body()
        self.assertRegex(body, r"if\s*\(\s*report\.relayed\s*\)\s*\{[^}]*RecordLobbySettings\(")
        self.assertEqual(body.count("RecordLobbySettings("), 1)

    def test_the_session_comes_from_the_context(self):
        body = relay_body()
        self.assertIn("GetSessionState().lobbySessionId", body)


class ClearedRecordIsReported(unittest.TestCase):
    def test_the_handler_is_installed_with_the_tcp_callbacks(self):
        body = tcp_callbacks_body()
        self.assertIn("SetLobbySettingsClearedHandler(", body)
        self.assertIn("FormatLobbySettingsCleared(", body)

    def test_context_runs_the_handler_after_releasing_its_lock(self):
        code = strip_comments(read("src/runtime/server/server_context.cpp"))
        for signature, reason in (("bool ServerContext::StartSession()", "session_start"),
                                  ("bool ServerContext::EndSession()", "session_end")):
            body = extract_braced_function(code, signature)
            self.assertIn(f'handler(cleared, "{reason}", sessionId)', body, signature)
            self.assertLess(body.index("lock.unlock()"), body.index("handler(cleared"), signature)
            self.assertLess(body.index("std::exchange(m_lobbySettings"), body.index("handler(cleared"), signature)


class SymbolsCarryTheValuesFromTheBinary(unittest.TestCase):
    def test_tcp_form_is_csymbol64_of_the_s_prefixed_name(self):
        text = read("src/abi/symbols.h")
        tcp = text[text.index("namespace Tcp {") :]
        self.assertRegex(tcp, r"SymbolId LobbySettingsResponse = 0x5b71b22a4483bda5;")

    def test_local_form_is_the_hash_of_the_game_literal(self):
        text = read("src/abi/symbols.h")
        top = text[: text.index("namespace Social {")] if "namespace Social {" in text else text
        self.assertRegex(top, r"SymbolId LobbySettingsResponse = 0x4cd5b97b2b97d9a3;")

    def test_the_peer_constant_is_the_one_read_from_the_exe(self):
        text = read("src/abi/echovr.h")
        self.assertRegex(text, r"Peer_AllPeers\s*=\s*0xFFFFFFFFFFFFFFFD;")
        header = read("src/runtime/server/lobby_settings_relay.h")
        self.assertRegex(header, r"kLobbySettingsAllPeers\s*=\s*0xFFFFFFFFFFFFFFFDULL;")


if __name__ == "__main__":
    unittest.main()
