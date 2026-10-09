#include "runtime/server/gameserver.h"
#include "runtime/server/gameserver_internal.h"

#include "core/globals.h"
#include "core/logging.h"
#include "runtime/lifecycle/service_config.h"  // NevrCfgGetFlat

// Telemetry streamer connection, split out of GameServerLib::RequestRegistration (#47).
void GameServerLib::ConnectTelemetry(const std::string& wsToken) {
  // Connect telemetry streamer if enabled
  if (g_telemetryEnabled && m_telemetry) {
    // OPTIONAL (N133 S4b): an absent telemetry.uri/token is a correct disabled
    // state, never fatal — NevrCfgGetFlat returns null/"" and the else-branch logs
    // "telemetry disabled". No ${VAR:?} is forced on these keys.
    const char* telemetryUri = NevrCfgGetFlat("telemetry_uri");
    const char* telemetryToken = NevrCfgGetFlat("telemetry_token");
    if (telemetryUri && telemetryUri[0] != '\0') {
      std::string token;
      if (telemetryToken && telemetryToken[0] != '\0') {
        token = telemetryToken;
      } else {
        // Fall back to cached auth token when telemetry_token not configured
        token = wsToken;
        // #114: that token expires; after an HTTP 401 on reconnect, mint a new one. A configured
        // telemetry_token is the operator's and is not refreshed.
        m_telemetry->SetBearerTokenRefresher([]() {
          std::string reason;  // the refresher has no operator to tell; each step already logged its cause
          return AcquireServerDbToken(reason);
        });
      }
      m_telemetry->Connect(std::string(telemetryUri), token);
    } else {
      // N124. Was Debug — off in production — so a server running without
      // telemetry was silent about it, and "deliberately disabled" looked
      // identical to "the telemetry code never ran". Exactly the asymmetry N122
      // found in UPnP: the healthy-but-off state has to be observable or an
      // operator cannot tell configuration from breakage.
      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] No telemetry_uri in config — telemetry disabled");
    }
  } else {
    // N124. The OUTER guard was silent too, and it is the commoner case: with
    // -notelemetry (or no streamer) the whole block is skipped, so the most usual
    // way telemetry ends up off produced no output at all. Found by writing a
    // smoke flagset that passed -notelemetry to observe the disabled state — and
    // thereby disabled the very branch that reports it.
    Log(EchoVR::LogLevel::Info,
        "[NEVR.GAMESERVER] telemetry disabled (enabled=%d streamer=%s) — not connecting",
        g_telemetryEnabled ? 1 : 0, m_telemetry ? "present" : "null");
  }
}
