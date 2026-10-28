"""#520: the bridge tells the game's sockets apart by what they asked for, not by when they connected.

Source-shape sensors for what the GTests cannot reach on their own. The decision (compat/socket_role.h) is
tested by test_socket_role; the real bridge by RolesByPathTest in test_behavioral; the redirect that gives
the config and login services their request targets by test_service_config. What no test target links is
config.cpp (the hook that builds the redirect) and the lines of ws_bridge.cpp that hand the Open handler's
request target to the decision, so those are pinned here:

  - the Open handler admits a socket with the upgrade request's target and the listener it arrived on;
  - both listeners register that handler, each with its own Listener;
  - the login session is read inside the critical section that takes the slot, and nowhere in the handler
    without g_pairsMutex;
  - the connect-order counter and the write-only login flag are gone;
  - the service redirect and the login override hand the game's JSON key to the redirect, which is what
    puts /config or /login on the bridge address;
  - the N91 row and the comment above it describe the guard that exists.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def bridge() -> str:
    return read("src/runtime/compat/ws_bridge.cpp")


def client_message_handler() -> str:
    return extract_braced_function(bridge(), "const auto makeOnClientMessage = [](nevr_socket_role::Listener listener)")


def admit_body() -> str:
    return extract_braced_function(bridge(), "static AdmittedSocket AdmitGameSocket(")


class OpenHandlerAdmitsByRequestTarget(unittest.TestCase):
    def test_the_open_handler_passes_the_upgrade_requests_target_and_its_listener(self):
        self.assertIn("AdmitGameSocket(&gameWs, msg->openInfo.uri, listener)", client_message_handler())

    def test_the_admission_takes_the_slot_from_the_decision_with_the_target_read_as_a_role(self):
        body = admit_body()
        self.assertRegex(body, r"g_roles\.Admit\(\s*listener\s*,\s*nevr_socket_role::RoleFromRequestTarget\(\s*target\s*\)\s*\)")

    def test_the_open_handler_does_not_branch_on_the_connection_index(self):
        handler = client_message_handler()
        self.assertNotRegex(handler, r"connIdx\s*(==|>=|<|>)\s*\d")
        self.assertIn("role == nevr_socket_role::Role::Login", handler)
        self.assertIn("role == nevr_socket_role::Role::Matchmaker", handler)

    def test_the_connect_order_counter_is_gone(self):
        code = strip_comments(bridge())
        self.assertNotIn("g_connectionCount", code)

    def test_each_listener_registers_the_handler_for_its_own_listener(self):
        code = strip_comments(bridge())
        self.assertRegex(
            code,
            r"g_server->setOnClientMessageCallback\(\s*makeOnClientMessage\(\s*nevr_socket_role::Listener::Service\s*\)\s*\)")
        self.assertRegex(
            code,
            r"g_matchServer->setOnClientMessageCallback\(\s*makeOnClientMessage\(\s*nevr_socket_role::Listener::Matchmaker\s*\)\s*\)")


class LoginSessionIsReadUnderTheLock(unittest.TestCase):
    def test_admission_reads_the_login_session_after_taking_the_mutex(self):
        body = admit_body()
        self.assertIn("g_pairsMutex", body)
        self.assertLess(body.index("g_pairsMutex"), body.index("g_loginRemoteWs"))
        self.assertIn("admitted.loginRemote = g_loginRemoteWs", body)

    def test_the_open_handler_uses_the_snapshot_not_the_global_to_share_the_session(self):
        handler = client_message_handler()
        self.assertIn("loginRemote->setOnMessageCallback", handler)
        self.assertNotRegex(handler, r"g_loginRemoteWs->")
        # The only other mentions are the writer, and the comparison in the remote callback; both under the lock.
        # Each mention sits after a g_pairsMutex acquisition in the same handler (the window runs from that lock).
        for match in re.finditer(r"g_loginRemoteWs", handler):
            lock = handler.rfind("lock_guard<std::mutex> lk(g_pairsMutex)", 0, match.start())
            self.assertNotEqual(lock, -1, "g_loginRemoteWs outside g_pairsMutex")


class TheN91GuardIsTheOneThatExists(unittest.TestCase):
    def test_no_write_only_login_flag_and_no_safety_net_that_is_not_there(self):
        self.assertNotIn("g_loginInjectedAnywhere", strip_comments(bridge()))
        self.assertNotIn("SAFETY NET", bridge())

    def test_only_the_login_role_injects(self):
        body = extract_braced_function(bridge(), "static void HandleRemoteOpen(")
        self.assertRegex(
            body, r"pair->role\s*==\s*nevr_socket_role::Role::Login\s*&&\s*!pair->loginInjected")

    def test_the_remote_open_handler_runs_the_flush_with_the_pair_it_opened_for(self):
        handler = client_message_handler()
        self.assertRegex(handler, r"HandleRemoteOpen\(\s*pairPtr\s*,\s*connIdx\s*,\s*discordId\s*,\s*accountName\s*,\s*bearerToken\s*\)")

    def test_the_flush_recheck_refuses_a_pair_that_took_a_freed_pairs_address(self):
        body = extract_braced_function(bridge(), "static void HandleRemoteOpen(")
        self.assertRegex(body, r"!PairRegisteredLocked\(pair\)\s*\|\|\s*pair->remoteWs\s*!=\s*remote")

    def test_a_frame_from_a_pair_sharing_the_remote_queues_behind_a_flushing_pair(self):
        body = extract_braced_function(bridge(), "static void ForwardGameFrameToRemote(")
        self.assertRegex(body, r"other\.remoteWs\s*==\s*pair->remoteWs\s*&&\s*!other\.remoteOpen")

    def test_a_first_frame_that_contradicts_the_role_is_checked_for_every_game_frame(self):
        self.assertIn("ForwardGameFrameToRemote(gameWs, msg->str, msg->binary)", client_message_handler())
        body = extract_braced_function(bridge(), "static void ForwardGameFrameToRemote(")
        self.assertIn("CheckFirstFrameRoleLocked(*pair, frame)", body)


class TheRedirectGivesTheServicesTheirRequestTargets(unittest.TestCase):
    def test_the_service_redirect_hands_the_json_key_to_the_redirect(self):
        body = extract_braced_function(read("src/runtime/lifecycle/config.cpp"), "static CHAR* RedirectServiceUrl(")
        self.assertRegex(
            body,
            r"NevrCfgRedirect\(\s*url\s*,\s*httpTarget\s*,\s*IsWebSocketBridgeActive\(\)\s*\?\s*1\s*:\s*0\s*,\s*GetWebSocketBridgePort\(\)\s*,\s*keyName\s*\)")

    def test_the_login_override_reroute_carries_the_key_to_the_relay_address(self):
        config = strip_comments(read("src/runtime/lifecycle/config.cpp"))
        self.assertRegex(
            config, r"ApplyLoginRedirectOverride\(\s*overrideInput\s*,\s*ResolveLoginOverrideBridgeUrl\s*,\s*keyName\s*\)")
        resolver = extract_braced_function(
            read("src/runtime/lifecycle/config.cpp"), "static const char* ResolveLoginOverrideBridgeUrl(")
        self.assertRegex(resolver, r"NevrCfgAutoRelay\(\s*bridgePort\s*,\s*static_cast<const char\*>\(\s*serviceKey\s*\)\s*\)")

    def test_the_accessor_tags_only_the_bridge_address(self):
        body = extract_braced_function(
            read("src/runtime/lifecycle/service_config.cpp"), "const char* NevrCfgRedirect(")
        self.assertRegex(body, r"if\s*\(\s*bridgeActive\s*!=\s*0\s*\)\s*return\s+InternCStr\(\s*nevr_socket_role::TagBridgeUrl\(")


class TheDocsDescribeTheGuard(unittest.TestCase):
    def test_the_n91_row_names_what_checks_it(self):
        rows = [line for line in read("AGENTS.md").splitlines() if line.startswith("| N91 ")]
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertIn("src/runtime/compat/socket_role.h", row)
        self.assertIn("src/runtime/tests/test_socket_role.cpp", row)
        self.assertIn("RolesByPathTest", row)
        self.assertIn("tools/tests/test_bridge_role_wiring.py", row)
        self.assertNotIn("records the gap", row)


if __name__ == "__main__":
    unittest.main()
