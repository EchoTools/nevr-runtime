#include "globals.h"

/// Remove extra console added by -headless for fully headless servers.
BOOL g_noConsole = FALSE;
/// Whether the game is booting in headless mode (no graphics/audio).
BOOL g_isHeadless = FALSE;
/// Whether the game should exit when the serverdb connection is lost.
/// If a round is active, the exit is deferred until the round ends (plus a 30-second grace period).
/// Default TRUE — suppress with -no-exit-on-error.
BOOL g_exitOnError = TRUE;
/// A dedicated server's tick rate (ticks/second). Set from config network.tick_rate_hz (default 120, range
/// 10..1000). Zero keeps the game's own fixed step (30 a second); a server is always fixed-timestep.
UINT32 g_headlessTickRateHz = 120;
/// Whether telemetry streaming is enabled (default TRUE). Disabled by -no-telemetry.
BOOL g_telemetryEnabled = TRUE;
/// Telemetry streaming rate in Hz (default 10). Set by -telemetry-rate <N>.
UINT32 g_telemetryRateHz = 30;
/// Telemetry diagnostic mode. Logs snapshot data instead of sending over WS.
BOOL g_telemetryDiag = FALSE;
/// Prefix all log lines with a high-resolution timestamp.
BOOL g_timestampLogs = FALSE;
/// Whether UPnP port forwarding is enabled. Set by -upnp or config "upnp".
BOOL g_upnpEnabled = FALSE;
/// External UPnP port override (0 = use broadcaster port). Set by config "upnp_port".
UINT16 g_upnpPort = 0;
/// Internal IP override for registration (empty = auto). Set by config "internal_ip".
CHAR g_internalIpOverride[46] = {};
/// External IP override for registration (empty = auto). Set by config "external_ip".
CHAR g_externalIpOverride[46] = {};
/// Arena round time override in seconds (0.0 = use game default 300s). Set by config "arena_round_time".
FLOAT g_arenaRoundTime = 0.0f;
/// Arena post-goal celebration time override in seconds (0.0 = use game default 15s). Set by config "arena_celebration_time".
FLOAT g_arenaCelebrationTime = 0.0f;
/// Arena mercy rule point spread override (0.0 = use game default 12). Set by config "arena_mercy_score".
FLOAT g_arenaMercyScore = 0.0f;
/// Whether the -noovr flag was passed (or implied by -server).
BOOL g_noOvr = FALSE;
