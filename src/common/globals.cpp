#include "globals.h"

/// Remove extra console added by -headless for fully headless servers.
BOOL g_noConsole = FALSE;
/// Whether the game is booting in headless mode (no graphics/audio).
BOOL g_isHeadless = FALSE;
/// Whether the game should exit when the serverdb connection is lost.
/// If a round is active, the exit is deferred until the round ends (plus a 30-second grace period).
BOOL g_exitOnError = FALSE;
/// Headless mode tick rate (ticks/second). Non-zero sets the override; zero removes throttling.
UINT32 g_headlessTickRateHz = 120;
/// Whether telemetry streaming is enabled (default TRUE). Disabled by -notelemetry.
BOOL g_telemetryEnabled = TRUE;
/// Telemetry streaming rate in Hz (default 10). Set by -telemetryrate <N>.
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
