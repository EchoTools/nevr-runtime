#include "runtime/lifecycle/cli.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"

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
  // Issue #22: this said "custom path to config.yaml", but the path itself is
  // loaded as the game's native JSON config (LoadLocalConfigHook), and only its
  // DIRECTORY is searched for config.yaml (service_config.cpp FindNevrConfigYamlPath).
  EchoVR::AddArgHelpString(pArgSyntax, "-config",
      "[NEVR] Path to a JSON file the game loads instead of _local/config.json; "
      "config.yaml is looked for in the same directory");

  EchoVR::AddArgSyntax(pArgSyntax, "-region", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-region", "[NEVR] Set the matchmaking region");

  EchoVR::AddArgSyntax(pArgSyntax, "-serverregion", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-serverregion", "[NEVR] Set the server fleet region");

  EchoVR::AddArgSyntax(pArgSyntax, "-noexitonerror", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-noexitonerror",
      "[NEVR] Keep server running after serverdb disconnect (default: exit)");

  EchoVR::AddArgSyntax(pArgSyntax, "-exitonerror", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-exitonerror",
      "[NEVR] Deprecated — exit-on-error is now the default");

  EchoVR::AddArgSyntax(pArgSyntax, "-traceexports", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-traceexports",
                           "[NEVR] Trace the export calls of platform DLLs: pnsrad, pnsovr, pnsdemo or all (comma "
                           "separated); logs [NEVR.TRACE] lines. Off by default. Read from the command line by "
                           "runtime/hook/export_tracer.cpp");

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

  EchoVR::AddArgSyntax(pArgSyntax, "-allow-dbgcore", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-allow-dbgcore",
      "[NEVR] Allow dbgcore.dll in the game directory (legacy injection)");

  // -config-path is NOT ignored: boot.cpp handles it exactly like -config.
  EchoVR::AddArgSyntax(pArgSyntax, "-config-path", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-config-path", "[NEVR] Same as -config");

  // Backwards compat: accept deprecated flags without error (they're silently ignored)
  EchoVR::AddArgSyntax(pArgSyntax, "-timestep", 1, 1, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-fixedtimestep", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-noovr", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-headless", 0, 0, FALSE);

  return result;
}
