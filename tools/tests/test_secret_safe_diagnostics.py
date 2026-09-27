"""Source contracts for response and transport diagnostic redaction."""

from pathlib import Path
import unittest


REPO = Path(__file__).resolve().parents[2]


def source(relative_path: str) -> str:
    return (REPO / relative_path).read_text(encoding="utf-8")


def section(text: str, start: str, end: str) -> str:
    start_at = text.index(start)
    end_at = text.index(end, start_at + len(start))
    return text[start_at:end_at]


class SecretSafeDiagnosticSourceTest(unittest.TestCase):
    def test_auth_response_logs_drop_bodies_and_parser_exception_text(self):
        refresh = source("plugins/common/include/auth_token_refresh.h")
        self.assertIn("LogHttpResponseSummary", refresh)
        self.assertNotIn("response.substr", refresh)
        self.assertNotIn("body=%s", refresh)
        self.assertNotIn("e.what()", refresh)
        self.assertIn("catch (const nlohmann::json::parse_error&)", refresh)

        token_auth = source("src/modules/token-auth/src/token_auth.cpp")
        request = section(token_auth, "std::string DeviceAuth::HttpPostPublic(",
                          "TokenAuth::DevicePollResponse DeviceAuth::PollDeviceCode(")
        self.assertIn("FormatRedactedUrlDiagnostic", request)
        self.assertIn("curl_code=%d", request)
        self.assertNotIn("curl_easy_strerror", request)
        self.assertNotIn("e.what()", section(token_auth, "std::string DeviceAuth::RequestDeviceCode(",
                                               "TokenAuth::DevicePollResponse DeviceAuth::PollDeviceCode("))
        self.assertIn("FormatRedactedUrlDiagnostic(\"[NEVR.AUTH] Configured: url=\"", token_auth)

        gameserver = source("src/runtime/server/gameserver.cpp")
        error_case = section(gameserver, "case gameservice::v1::Envelope::kError:", "default:")
        self.assertIn("message_bytes=%zu", error_case)
        self.assertNotIn("error.message().c_str()", error_case)
        auth = section(gameserver, "static std::string AuthenticateServer() {", "// N133 S4b: all NEVR-key reads")
        self.assertIn("LogHttpResponseSummary", auth)
        self.assertNotIn("response.substr", auth)
        self.assertNotIn("curl_easy_strerror", auth)
        self.assertIn("catch (const std::exception&)", auth)

    def test_transport_reasons_and_curl_text_are_not_logged(self):
        websocket = source("src/runtime/server/websocket_client.cpp")
        telemetry = source("src/runtime/server/telemetry_streamer.cpp")
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        winhttp = source("src/runtime/compat/winhttp_stub.cpp")
        asset_cdn = source("src/runtime/patch/asset_cdn.cpp")

        for path_text in (websocket, telemetry, bridge):
            self.assertNotIn("closeInfo.reason.c_str()", path_text)
            self.assertNotIn("errorInfo.reason.c_str()", path_text)
        self.assertNotIn("msg->str.c_str()", websocket)
        self.assertIn("FormatWebSocketCloseDiagnostic", websocket)
        self.assertIn("FormatWebSocketErrorDiagnostic", websocket)
        self.assertIn("FormatWebSocketCloseDiagnostic", telemetry)
        self.assertIn("FormatWebSocketErrorDiagnostic", telemetry)
        self.assertIn("http_status=%d retries=%u", bridge)
        self.assertNotIn("errMsg.c_str()", bridge)
        self.assertNotIn("what=%s", bridge)
        self.assertEqual(bridge.count("FormatBindFailureDiagnostic"), 2)
        self.assertNotIn("closeInfo.reason.c_str()", bridge)
        self.assertNotIn("errorInfo.reason.c_str()", bridge)

        for path_text in (winhttp, asset_cdn, websocket, telemetry, bridge,
                          source("src/modules/token-auth/src/token_auth.cpp"),
                          source("src/runtime/server/gameserver.cpp"),
                          source("plugins/common/include/auth_token_refresh.h")):
            self.assertNotIn("curl_easy_strerror", path_text)
        self.assertIn("curl_code=", winhttp)
        self.assertIn("FormatCurlFailureDiagnostic", asset_cdn)
        fetch_manifest = section(asset_cdn, "bool AssetCDN::FetchManifest() {", "// Validate version")
        self.assertIn("curl_easy_setopt(curl, CURLOPT_URL, MANIFEST_URL)", fetch_manifest)
        self.assertIn("manifest fetch failed: http_status=%ld", fetch_manifest)
        self.assertNotIn("MANIFEST_URL);", fetch_manifest[fetch_manifest.index("if (http_code != 200)"):])
        self.assertIn("catch (const json::parse_error&)", fetch_manifest)
        self.assertIn("Manifest JSON parse error response_bytes=%zu", fetch_manifest)
        self.assertNotIn("e.what()", fetch_manifest)

    def test_login_failure_is_numeric_and_declared_length_bounded(self):
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        parser = section(bridge, "std::optional<LoginFailureDiagnostic> ReadLoginFailureDiagnostic(",
                         "}  // namespace")
        self.assertIn("payloadLength <=", parser)
        self.assertIn("numeric_limits<size_t>::max() - kEnvelopeHeaderSize", parser)
        self.assertIn("payloadSize > frame.size() - kEnvelopeHeaderSize", parser)
        self.assertNotIn("errMsg", bridge)
        self.assertNotIn("%.*s", bridge)
        self.assertIn("FormatLoginFailureDiagnostic", bridge)
        self.assertIn("gameWsPtr->sendBinary(fakeSuccess);", bridge)
        self.assertIn("target->sendBinary(rmsg->str);", bridge)
        self.assertIn("server->game", bridge)

    def test_callback_exception_sentinel_is_removed_by_actual_guard(self):
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        guard = section(bridge, "static auto GuardWsCallback(", "namespace {")
        self.assertIn("catch (const std::exception&)", guard)
        self.assertIn("FormatCallbackFailureDiagnostic", guard)
        self.assertNotIn("e.what()", guard)
        self.assertIn('throw std::runtime_error("response-secret-sentinel")', bridge)

    def test_smoke_markers_and_logging_standard_match_numeric_diagnostics(self):
        smoke = source("tests/smoke/score-log.sh")
        self.assertIn("C03@@client@@token_auth configured@@\\[NEVR\\.AUTH\\] Configured: url=", smoke)
        self.assertIn("C08@@client@@Access token refreshed@@Token refreshed successfully@@[Tt]oken refresh failed", smoke)
        self.assertIn("C11@@client@@HTTP served through the curl bridge@@", smoke)
        self.assertIn("S20@@server@@Service accepted the login@@", smoke)
        self.assertIn("S30@@server@@Telemetry stream connected@@", smoke)

        logging_doc = source("docs/standards/logging.md")
        policy = section(logging_doc, "### Rule 4: Remote text and response bodies are not log fields", "---")
        self.assertIn("Do not log these values", policy)
        self.assertIn("numeric", policy)
        self.assertEqual(logging_doc.count("### Rule 4:"), 1)
        config_policy = section(logging_doc, "### Rule 11: Config decisions logged at load time", "### Rule 12:")
        self.assertIn("not secret values or unredacted URLs", " ".join(config_policy.split()))
        self.assertIn('"[WEBSOCKET] Disconnected from ServerDB (code: %u) reconnect_count=%u"', logging_doc)
        self.assertIn('"[NEVR.WS] login failed status=%llu message_bytes=%zu"', logging_doc)


if __name__ == "__main__":
    unittest.main()
