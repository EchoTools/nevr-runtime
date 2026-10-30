"""#1254: a refused login stops a server with the cause and is said once by a client.

Source-shape sensors for what the GTests do not reach by themselves. The decisions (compat/login_refusal.h) are
tested by test_login_refusal; the real bridge, legacy and router, by RefusedLogin* in test_behavioral. What is
pinned here is the order and the plumbing a behavioural test would only notice after the fact:

  - PrepareRemote stops a server with no credential before it makes the remote, and hands the cause to
    ServerFatal as an argument, never as the format;
  - the legacy remote's Error case reports the status before it closes the game's sockets;
  - the router transport reports the status to the bridge before it retires the remote, and the bridge installs
    the hook;
  - the config line names the config the loader found (service_config.cpp), and the accessor is declared;
  - the test is built and run by the recipes that build and run the others.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def bridge() -> str:
    return strip_comments(read("src/runtime/compat/ws_bridge.cpp"))


def prepare_remote() -> str:
    return extract_braced_function(bridge(), "static PreparedRemote PrepareRemote(")


class ServerWithNoCredentialStopsBeforeConnecting(unittest.TestCase):
    def test_the_stop_comes_before_the_remote_is_made(self):
        body = prepare_remote()
        stop = body.index("NoCredentialCause(")
        made = body.index("std::make_shared<ix::WebSocket>()")
        self.assertLess(stop, made)

    def test_the_stop_is_for_a_server_only(self):
        body = prepare_remote()
        self.assertRegex(body, r"if \(g_isServer\) \{\s*const char\* cfgDiscordId")

    def test_the_stop_returns_no_remote(self):
        body = prepare_remote()
        stop = body.index("NoCredentialCause(")
        self.assertRegex(body[stop:stop + 300], r"ServerFatal\(\"%s\", cause\.c_str\(\)\);\s*return PreparedRemote\{\};")

    def test_the_cause_is_never_the_format_string(self):
        code = bridge()
        self.assertNotRegex(code, r"ServerFatal\(\s*cause")
        self.assertEqual(len(re.findall(r'ServerFatal\("%s", cause\.c_str\(\)\)', code)), 3)

    def test_a_client_is_not_held_back_by_the_credential_rule(self):
        body = prepare_remote()
        self.assertNotRegex(body, r"if \(!g_isServer\) \{\s*const char\* cfgDiscordId")

    def test_a_latched_sign_in_makes_no_remote_for_a_client_only(self):
        body = prepare_remote()
        self.assertRegex(body, r"if \(!g_isServer && ClientRefusedAlready\(refusalKey\)\) \{[^}]*return PreparedRemote\{\};")


class ServerWithNoPasswordSignsInByDeviceCode(unittest.TestCase):
    """#1532: no auth.password is a device sign-in, not a missing key; no token from it stops before any remote."""

    def test_the_no_token_stop_comes_before_the_remote_is_made(self):
        body = prepare_remote()
        stop = body.index("NoSignInCause(")
        made = body.index("std::make_shared<ix::WebSocket>()")
        self.assertLess(stop, made)
        self.assertRegex(body[stop:stop + 200], r"ServerFatal\(\"%s\", cause\.c_str\(\)\);\s*return PreparedRemote\{\};")

    def test_the_stop_asks_token_auth_for_the_token_only_for_a_no_password_server(self):
        body = prepare_remote()
        self.assertRegex(
            body,
            r"ServerSignsInByDeviceCode\(password\) && ReadLoginIdentity\(false\)\.bearerToken\.empty\(\)")

    def test_a_server_with_no_password_runs_the_token_auth_flow_with_a_console_prompt(self):
        token_auth = strip_comments(read("src/modules/token-auth/src/token_auth.cpp"))
        init = extract_braced_function(token_auth, "void nevr_token_auth::Init(")
        self.assertIn("serverSignsInByDeviceCode", init)
        self.assertIn("RunDeviceAuthFlowOffBootstrapThread(*s_auth, serverSignsInByDeviceCode)", init)
        self.assertIn("Server mode disables device-code authentication", init)  # the password path is unchanged

    def test_an_error_exit_of_a_server_holds_for_the_host(self):
        crash = strip_comments(read("src/runtime/lifecycle/crash_recovery.cpp"))
        body = extract_braced_function(crash, "void ServerFatal(")
        self.assertLess(body.index("HoldForHostBeforeExit("), body.index("ForceFatalExit(1)"))


class RefusalIsReportedBeforeTheSocketsAreClosed(unittest.TestCase):
    def test_the_legacy_error_case_reports_the_status_then_closes(self):
        code = bridge()
        report = code.index("OnRemoteLoginRefused(rmsg->errorInfo.http_status, refusalKey, connIdx);")
        error = code.rindex("case ix::WebSocketMessageType::Error:", 0, report)
        close = code.index("CloseGameSocketsForRemote(remoteAddress, connIdx, 0);", report)
        self.assertLess(error, report)
        self.assertLess(report, close)
        self.assertEqual(code.count("OnRemoteLoginRefused(rmsg->errorInfo.http_status"), 1)

    def test_the_legacy_open_handler_ends_a_socket_with_no_remote(self):
        code = bridge()
        self.assertRegex(
            code,
            r"PreparedRemote prepared = PrepareRemote\(role, connIdx\);\s*"
            r"(?:if \(prepared\.answerLocally\) \{\s*AnswerLoginLocally\(&gameWs, connIdx\);\s*break;\s*\}\s*)?"
            r"if \(!prepared\.remote\) \{\s*AbandonGameSocket\(&gameWs, role, connIdx\);\s*break;\s*\}")

    def test_the_legacy_open_handler_answers_a_token_less_login_before_it_ends_a_socket(self):
        # #1446: a login whose sign-in ended without a token is answered (LoginFailure) before the no-remote branch,
        # so the game is not closed into a reconnect loop. Either order of the two branches fails this check.
        code = bridge()
        self.assertRegex(
            code,
            r"PreparedRemote prepared = PrepareRemote\(role, connIdx\);\s*"
            r"if \(prepared\.answerLocally\) \{\s*AnswerLoginLocally\(&gameWs, connIdx\);\s*break;\s*\}\s*"
            r"if \(!prepared\.remote\) \{\s*AbandonGameSocket\(&gameWs, role, connIdx\);\s*break;\s*\}")

    def test_an_abandoned_login_gives_its_slot_back(self):
        body = extract_braced_function(bridge(), "static void AbandonGameSocket(")
        self.assertIn("g_roles.LoginSessionEnded()", body)

    def test_the_refusal_key_is_the_sign_in_not_the_connection(self):
        body = prepare_remote()
        self.assertIn("const std::string refusalKey = remoteBearer;", body)
        self.assertLess(body.index("SelectRemoteBearer("), body.index("ClientRefusedAlready(refusalKey)"))

    def test_the_key_is_held_per_remote_not_per_connection_index(self):
        code = strip_comments(bridge())
        self.assertIn("g_routerRefusalKeys[request.remote] = prepared.refusalKey;", code)
        self.assertIn("g_routerRefusalKeys.find(request.remote)", code)
        self.assertNotIn("g_routerRefusalKeys[request.connIdx]", code)

    def test_only_an_auth_status_is_a_refusal(self):
        body = extract_braced_function(bridge(), "static void OnRemoteLoginRefused(")
        self.assertIn("if (!nevr_login_refusal::IsLoginRefusalStatus(httpStatus)) return;", body)

    def test_a_server_stop_names_the_config(self):
        body = extract_braced_function(bridge(), "static void OnRemoteLoginRefused(")
        self.assertIn("RefusedLoginCause(httpStatus, NevrCfgSource(), deviceSignIn)", body)


class RouterReportsTheStatusToTheBridge(unittest.TestCase):
    def test_the_transport_reports_before_it_retires_the_remote(self):
        code = strip_comments(read("src/runtime/compat/ws_session_transport.cpp"))
        error = code.index("case ix::WebSocketMessageType::Error:")
        report = code.index("if (hooks_.refused) hooks_.refused(request, message->errorInfo.http_status);", error)
        retire = code.index("Retire(remote);", error)
        self.assertLess(report, retire)

    def test_the_bridge_installs_the_hook(self):
        self.assertRegex(bridge(), r"hooks\.refused = \[\]\(const nevr_session_router::RemoteOpenRequest& request, int httpStatus\)")

    def test_the_hook_is_declared_on_the_transports_hooks(self):
        header = strip_comments(read("src/runtime/compat/ws_session_transport.h"))
        self.assertIn("std::function<void(const nevr_session_router::RemoteOpenRequest&, int httpStatus)> refused;", header)


class TheConfigLineNamesTheConfigTheLoaderFound(unittest.TestCase):
    def test_the_loader_records_where_it_looked(self):
        code = strip_comments(read("src/runtime/lifecycle/service_config.cpp"))
        self.assertIn("ConfigSourceStore() = nevr_login_refusal::DescribeConfigSource(choice.path, searched);", code)
        self.assertIn("searched.push_back(choice.root_path);", code)

    def test_a_rejected_config_is_said_to_be_rejected(self):
        code = strip_comments(read("src/runtime/lifecycle/service_config.cpp"))
        self.assertIn('ConfigSourceStore() = path + " (rejected; built-in defaults in use)";', code)

    def test_the_accessor_is_declared(self):
        self.assertIn("const char* NevrCfgSource();", strip_comments(read("src/runtime/lifecycle/service_config.h")))

    def test_no_unregistered_log_tag_is_introduced(self):
        for path in ("src/runtime/compat/login_refusal.h", "src/runtime/compat/ws_bridge.cpp"):
            self.assertNotIn("NEVR.LOGIN", read(path), path)


class TheTestIsBuiltAndRun(unittest.TestCase):
    def test_cmake_builds_it(self):
        cmake = read("src/runtime/CMakeLists.txt")
        self.assertIn("add_executable(test_login_refusal tests/test_login_refusal.cpp)", cmake)
        self.assertIn("gtest_discover_tests(test_login_refusal", cmake)

    def test_the_unit_recipe_builds_and_runs_it(self):
        justfile = read("justfile")
        self.assertIn("--target test_login_refusal", justfile)
        self.assertRegex(justfile, r"for test_name in [^\n]*\btest_login_refusal\b")


if __name__ == "__main__":
    unittest.main()
