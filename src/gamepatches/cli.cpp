#include "cli.h"
#include "common/echovr_functions.h"
#include "common/logging.h"

BOOL g_isServer = FALSE;
BOOL g_isOffline = FALSE;
BOOL g_isWindowed = FALSE;
CHAR g_customConfigPath[MAX_PATH] = {0};
CHAR g_regionOverride[64] = {0};

UINT64 BuildCmdLineSyntaxDefinitionsHook(PVOID pGame, PVOID pArgSyntax) {
  // Add all original CLI argument options.
  UINT64 result = EchoVR::BuildCmdLineSyntaxDefinitions(pGame, pArgSyntax);

  // NEVR-specific arguments
  EchoVR::AddArgSyntax(pArgSyntax, "-server", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-server", "[NEVR] Run as a dedicated game server (implies headless)");

  EchoVR::AddArgSyntax(pArgSyntax, "-offline", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-offline", "[NEVR] Run the game in offline mode");

  EchoVR::AddArgSyntax(pArgSyntax, "-windowed", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-windowed", "[NEVR] Run the game with no headset, in a window");

  EchoVR::AddArgSyntax(pArgSyntax, "-noconsole", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-noconsole", "[NEVR] Disable console window creation");

  EchoVR::AddArgSyntax(pArgSyntax, "-config", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-config", "[NEVR] Specify a custom path to config.yaml");

  EchoVR::AddArgSyntax(pArgSyntax, "-region", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-region", "[NEVR] Set the matchmaking region");

  EchoVR::AddArgSyntax(pArgSyntax, "-serverregion", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-serverregion", "[NEVR] Set the server fleet region");

  EchoVR::AddArgSyntax(pArgSyntax, "-exitonerror", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-exitonerror",
      "[NEVR] Exit server when serverdb connection is lost");

  EchoVR::AddArgSyntax(pArgSyntax, "-notelemetry", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-notelemetry", "[NEVR] Disable telemetry streaming");

  EchoVR::AddArgSyntax(pArgSyntax, "-telemetryrate", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-telemetryrate", "[NEVR] Set telemetry rate in Hz (default 10)");

  EchoVR::AddArgSyntax(pArgSyntax, "-telemetrydiag", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-telemetrydiag", "[NEVR] Log telemetry diagnostics every second");

  EchoVR::AddArgSyntax(pArgSyntax, "-timestamps", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-timestamps", "[NEVR] Prefix log lines with timestamps");

  EchoVR::AddArgSyntax(pArgSyntax, "-upnp", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-upnp", "[NEVR] Enable UPnP port forwarding");

  // Backwards compat: accept deprecated flags without error (they're silently ignored)
  EchoVR::AddArgSyntax(pArgSyntax, "-timestep", 1, 1, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-fixedtimestep", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-noovr", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-headless", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-config-path", 1, 1, FALSE);

  return result;
}
