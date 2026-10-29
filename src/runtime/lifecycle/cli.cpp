#include "runtime/lifecycle/cli.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"

BOOL g_isServer = FALSE;
BOOL g_isOffline = FALSE;
BOOL g_isWindowed = FALSE;
CHAR g_customConfigPath[MAX_PATH] = {0};
ConfigYamlFlags g_configYamlFlags;
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

  EchoVR::AddArgSyntax(pArgSyntax, "-no-console", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-no-console", "[NEVR] Disable console window creation");

  EchoVR::AddArgSyntax(pArgSyntax, "-config", 1, 1, FALSE);
  // Issue #22: this said "custom path to config.yaml", but the path is loaded as the game's native JSON
  // config (LoadLocalConfigHook). It does not decide where config.yaml is read (#1255: -config-file).
  EchoVR::AddArgHelpString(pArgSyntax, "-config",
      "[NEVR] Path to a JSON file the game loads instead of _local/config.json");

  EchoVR::AddArgSyntax(pArgSyntax, "-config-file", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-config-file",
      "[NEVR] Path to the config.yaml to read instead of the one in the nevr root (nevr/echovr under LocalAppData). "
      "A file that does not exist ends the run with exit code 64");

  EchoVR::AddArgSyntax(pArgSyntax, "-region", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-region", "[NEVR] Set the matchmaking region");

  EchoVR::AddArgSyntax(pArgSyntax, "-serverregion", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-serverregion", "[NEVR] Set the server fleet region");

  EchoVR::AddArgSyntax(pArgSyntax, "-no-exit-on-error", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-no-exit-on-error",
      "[NEVR] Keep server running after serverdb disconnect (default: exit)");

  EchoVR::AddArgSyntax(pArgSyntax, "-exit-on-error", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-exit-on-error",
      "[NEVR] No effect: exiting on error is the default");

  EchoVR::AddArgSyntax(pArgSyntax, "-trace-exports", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-trace-exports",
                           "[NEVR] Trace the export calls of platform DLLs: pnsrad, pnsovr, pnsdemo or all (comma "
                           "separated); logs [NEVR.TRACE] lines. Off by default. Read from the command line by "
                           "runtime/hook/export_tracer.cpp");

  EchoVR::AddArgSyntax(pArgSyntax, "-no-telemetry", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-no-telemetry", "[NEVR] Disable telemetry streaming");

  EchoVR::AddArgSyntax(pArgSyntax, "-telemetry-rate", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-telemetry-rate", "[NEVR] Set telemetry rate in Hz (default 10)");

  EchoVR::AddArgSyntax(pArgSyntax, "-telemetry-diag", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-telemetry-diag", "[NEVR] Log telemetry diagnostics every second");

  EchoVR::AddArgSyntax(pArgSyntax, "-timestamps", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-timestamps", "[NEVR] Prefix log lines with timestamps");

  EchoVR::AddArgSyntax(pArgSyntax, "-upnp", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-upnp", "[NEVR] Enable UPnP port forwarding");

  EchoVR::AddArgSyntax(pArgSyntax, "-require-signed-plugins", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-require-signed-plugins",
      "[NEVR] Load only plugins signed through the release pipeline; refuse the rest (default: unsigned plugins load). "
      "Read before the first plugin loads (runtime/lifecycle/boot.cpp, runtime/ext/plugin_trust.cpp)");

  // -config-path is registered so the game's parser accepts the token, and refused by the runtime (#1255): boot.cpp
  // ends the run with exit code 64 and the hint "use -config-file <file>". It has no help line: it is not an option.
  EchoVR::AddArgSyntax(pArgSyntax, "-config-path", 1, 1, FALSE);


  // #697: the names these flags had before they were kebab-case. The game's parser rejects a token with no
  // syntax entry (ValidateMainArgs, echovr.exe 0x1400da830), so each stays registered, with no help string so
  // `--help` never lists it. boot.cpp maps each to its new flag and warns once per run (core/cli_flags.h).
  EchoVR::AddArgSyntax(pArgSyntax, "-noconsole", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-noexitonerror", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-exitonerror", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-notelemetry", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-telemetryrate", 1, 1, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-telemetrydiag", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-traceexports", 1, 1, FALSE);
  // #875: the removed -skip-dbgcore-check and -allow-dbgcore stay registered, with no help line, so the game's
  // parser accepts them and the dbgcore.dll check in boot.cpp refuses the start on any command line.
  EchoVR::AddArgSyntax(pArgSyntax, "-skip-dbgcore-check", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-allow-dbgcore", 0, 0, FALSE);

  // GameMainWrapperHook removes -timestep and -fixedtimestep from the command line before the game parses
  // it when the line holds an option token exactly `-server` (crash_recovery.cpp, #515). They still reach
  // the game on a client, when the removal fails (out of memory) and when NEVR's reading of -server
  // differs from the game's (a quoted "-server"). ValidateMainArgs (echovr.exe 0x1400da830) rejects a token with no syntax entry as "not a valid
  // command", so the entries stay. -fixedtimestep is native (0x1404e7d60); -timestep is not, and a
  // client's game ignores it once it parses.
  EchoVR::AddArgSyntax(pArgSyntax, "-timestep", 1, 1, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-fixedtimestep", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-noovr", 0, 0, FALSE);
  EchoVR::AddArgSyntax(pArgSyntax, "-headless", 0, 0, FALSE);

  return result;
}
