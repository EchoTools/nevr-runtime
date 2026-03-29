#include "cli.h"
#include "common/echovr_functions.h"
#include "common/logging.h"

/// <summary>
/// A CLI argument flag indicating whether the game is booting as a dedicated server.
/// </summary>
BOOL g_isServer = FALSE;
/// <summary>
/// A CLI argument flag indicating whether the game is booting as an offline client.
/// </summary>
BOOL g_isOffline = FALSE;
/// <summary>
/// A CLI argument flag indicating whether the game is booting in a windowed mode, rather than with a VR headset.
/// </summary>
BOOL g_isWindowed = FALSE;

/// <summary>
/// Indicates whether the game was launched with `-noovr`.
/// </summary>
BOOL g_isNoOVR = FALSE;

/// <summary>
/// Custom config.json path provided via command-line argument. If empty, the default path is used.
/// </summary>
CHAR g_customConfigJsonPath[MAX_PATH] = {0};

/// <summary>
/// A timestep value in ticks/updates per second, to be used for headless mode (due to lack of GPU/refresh rate
/// throttling). If non-zero, sets the timestep override by the given tick rate per second. If zero, removes tick rate
/// throttling.
/// </summary>
UINT32 g_headlessTimeStep = 120;

/// <summary>
/// A detour hook for the game's method it uses to build CLI argument definitions.
/// Adds additional definitions to the structure, so that they may be parsed successfully without error.
/// </summary>
/// <param name="game">A pointer to the game instance.</param>
/// <param name="pArgSyntax">A pointer to the CLI argument structure tracking all CLI arguments.</param>
UINT64 BuildCmdLineSyntaxDefinitionsHook(PVOID pGame, PVOID pArgSyntax) {
  // Add all original CLI argument options.
  UINT64 result = EchoVR::BuildCmdLineSyntaxDefinitions(pGame, pArgSyntax);

  // Add our additional options
  EchoVR::AddArgSyntax(pArgSyntax, "-server", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-server", "[NEVR] Run as a dedicated game server");

  EchoVR::AddArgSyntax(pArgSyntax, "-offline", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-offline", "[NEVR] Run the game in offline mode");

  EchoVR::AddArgSyntax(pArgSyntax, "-windowed", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-windowed", "[NEVR] Run the game with no headset, in a window");

  EchoVR::AddArgSyntax(pArgSyntax, "-timestep", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-timestep",
                           "[NEVR] Sets the fixed update interval when using -headless (in ticks/updates per "
                           "second). 0 = no fixed time step, 120 = default");

  EchoVR::AddArgSyntax(pArgSyntax, "-noconsole", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-noconsole",
                           "[NEVR] Disable console window creation (must be used with -headless)");

  EchoVR::AddArgSyntax(pArgSyntax, "-config-path", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-config-path", "[NEVR] Specify a custom path to the config.json file");

  EchoVR::AddArgSyntax(pArgSyntax, "-exitonerror", 0, 0, FALSE);
  EchoVR::AddArgHelpString(
      pArgSyntax, "-exitonerror",
      "[NEVR] Exit server when serverdb connection is lost (deferred to end of round + 30s if round is active)");

  EchoVR::AddArgSyntax(pArgSyntax, "-notelemetry", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-notelemetry", "[NEVR] Disable telemetry streaming to telemetry server");

  EchoVR::AddArgSyntax(pArgSyntax, "-telemetryrate", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-telemetryrate",
                           "[NEVR] Set telemetry streaming rate in Hz (default 10)");

  EchoVR::AddArgSyntax(pArgSyntax, "-telemetrydiag", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-telemetrydiag",
                           "[NEVR] Log telemetry snapshot diagnostics (pointer chain, values) every second");

  EchoVR::AddArgSyntax(pArgSyntax, "-timestamps", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-timestamps",
                           "[NEVR] Prefix all log lines with high-resolution timestamps");

  EchoVR::AddArgSyntax(pArgSyntax, "-upnp", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-upnp",
                           "[NEVR] Enable UPnP port forwarding for the broadcaster UDP port");

  return result;
}
