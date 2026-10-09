"""Source contract for secret-safe URL diagnostics across runtime log sites."""

from pathlib import Path
import unittest


REPO = Path(__file__).resolve().parents[2]


class UrlDiagnosticLogInvariantTest(unittest.TestCase):
    def assert_source_contains(self, relative_path: str, *needles: str) -> None:
        source = (REPO / relative_path).read_text(encoding="utf-8")
        for needle in needles:
            with self.subTest(path=relative_path, needle=needle):
                self.assertIn(needle, source)

    def test_gameserver_url_diagnostics_use_full_redactor(self):
        source = (REPO / "src/runtime/server/gameserver.cpp").read_text(encoding="utf-8")
        self.assertNotIn("RedactPasswordInUri", source)
        self.assertIn("[NEVR.GAMESERVER] constructed serverdb URI for token auth:", source)
        self.assertIn("[NEVR.GAMESERVER] failed to initiate WebSocket connection uri=", source)
        self.assertIn("LogDiagnostics::FormatRedactedUrlDiagnostic(", source)

    def test_server_connection_logs_use_formatted_redaction(self):
        self.assert_source_contains(
            "src/runtime/server/websocket_client.cpp",
            'LogDiagnostics::FormatRedactedUrlDiagnostic("[NEVR.SERVERDB] Connecting to ServerDB at ", uri)',
        )
        self.assert_source_contains(
            "src/runtime/server/telemetry_streamer.cpp",
            'LogDiagnostics::FormatRedactedUrlDiagnostic("[NEVR.TELEMETRY] Connecting to ", uri)',
        )

    def test_bridge_remote_url_logs_use_formatted_redaction(self):
        self.assert_source_contains(
            "src/runtime/compat/ws_bridge.cpp",
            'LogDiagnostics::FormatRedactedUrlDiagnostic(\n                  "[NEVR.WS] Matchmaker conn=" + std::to_string(connIdx) + " using protobuf URL: "',
            'LogDiagnostics::FormatRedactedUrlDiagnostic(\n                          "[NEVR.WS] Remote open (conn=" + std::to_string(connIdx) + ", " + ConnLabel(connIdx) + "): "',
            'LogDiagnostics::FormatRedactedUrlDiagnostic("[NEVR.WS] Proxy remote target: "',
            'LogDiagnostics::FormatRedactedUrlPairDiagnostic(\n      "[NEVR.WS] Proxy listening on "',
        )

    def test_config_redirect_relay_and_override_logs_use_formatted_redaction(self):
        self.assert_source_contains(
            "src/runtime/lifecycle/config.cpp",
            'LogDiagnostics::FormatRedactedUrlDiagnostic(\n        "[NEVR.PATCH] Service override ["',
            'LogDiagnostics::FormatRedactedUrlPairDiagnostic(\n      "[NEVR.PATCH] auto-relay ["',
            'LogDiagnostics::FormatRedactedUrlPairDiagnostic(\n          "[NEVR.PATCH] HTTP(S) connection redirected: "',
            'LogDiagnostics::FormatRedactedUrlPairDiagnostic(\n      "[NEVR.PATCH] service redirect key="',
            'LogDiagnostics::FormatRedactedUrlPairDiagnostic(\n          "[NEVR.PATCH] config override key="',
        )

    def test_json_lookup_pre_redirect_path_does_not_log_or_parse_urls(self):
        source = (REPO / "src/runtime/lifecycle/config.cpp").read_text(encoding="utf-8")
        start = source.index("CHAR* JsonValueAsStringHook(")
        redirect_call = source.index("result = RedirectServiceUrl(keyName, result);", start)
        pre_redirect = source[start:redirect_call]
        self.assertNotIn("Log(", pre_redirect)
        self.assertNotIn("UrlDiagnostic", pre_redirect)
        hook = source[start:]
        self.assertIn(
            "const BOOL mayReadOverride = g_earlyConfigPtr != NULL && keyName != NULL &&\n"
            "                               root != g_earlyConfigPtr && result == defaultValue;",
            hook,
        )
        self.assertIn("if (mayReadOverride) override = EchoVR::JsonValueAsString", hook)
        self.assertIn("nevr::lifecycle::ApplyLoginRedirectOverride(overrideInput", hook)
        self.assertIn(
            "if (overrideOutcome.action == nevr::lifecycle::LoginRedirectOverrideAction::UseOverride)",
            hook,
        )
        self.assertIn("LogDiagnostics::FormatRedactedUrlPairDiagnostic(", hook)
        self.assertIn("overrideOutcome.value ? overrideOutcome.value : \"\"", hook)
        self.assertIn("return const_cast<CHAR*>(overrideOutcome.value);", hook)
        self.assertIn("Log(EchoVR::LogLevel::Info, \"%s\", diagnostic.c_str());", hook)


if __name__ == "__main__":
    unittest.main()
