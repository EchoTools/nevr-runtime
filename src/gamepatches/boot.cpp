#include "boot.h"
#include "cli.h"
#include "config.h"
#include "mode_patches.h"
#include "plugin_loader.h"
#include "patch_addresses.h"
#include "builtin_server_timing.h"
#include "builtin_token_auth.h"
#include "common/globals.h"
#include "common/logging.h"
#include "common/echovr_functions.h"

#include <cstdlib>
#include <shellapi.h>

/// <summary>
/// A detour hook for the game's command line pre-processing method, used to parse command line arguments.
/// </summary>
/// <param name="pGame">A pointer to the game instance.</param>
UINT64 PreprocessCommandLineHook(PVOID pGame) {
  // Parse command line arguments.
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  for (int i = 0; i < argc; ++i) {
    const LPWSTR arg = argv[i];

    if (lstrcmpW(arg, L"-server") == 0) {
      g_isServer = TRUE;
    } else if (lstrcmpW(arg, L"-offline") == 0) {
      g_isOffline = TRUE;
    } else if (lstrcmpW(arg, L"-noconsole") == 0) {
      g_noConsole = TRUE;
    } else if (lstrcmpW(arg, L"-windowed") == 0) {
      g_isWindowed = TRUE;
    } else if (lstrcmpW(arg, L"-exitonerror") == 0) {
      g_exitOnError = TRUE;
    } else if (lstrcmpW(arg, L"-notelemetry") == 0) {
      g_telemetryEnabled = FALSE;
    } else if (lstrcmpW(arg, L"-telemetryrate") == 0) {
      if (i + 1 < argc) {
        g_telemetryRateHz = std::wcstoul(argv[i + 1], nullptr, 10);
        if (g_telemetryRateHz == 0) g_telemetryRateHz = 10;
        ++i;
      }
    } else if (lstrcmpW(arg, L"-telemetrydiag") == 0) {
      g_telemetryDiag = TRUE;
    } else if (lstrcmpW(arg, L"-timestamps") == 0) {
      g_timestampLogs = TRUE;
    } else if (lstrcmpW(arg, L"-upnp") == 0) {
      g_upnpEnabled = TRUE;
    } else if (lstrcmpW(arg, L"-config") == 0 || lstrcmpW(arg, L"-config-path") == 0) {
      if (i + 1 < argc) {
        WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, g_customConfigPath, MAX_PATH, NULL, NULL);
        ++i;
      }
    } else if (lstrcmpW(arg, L"-region") == 0 || lstrcmpW(arg, L"-serverregion") == 0) {
      if (i + 1 < argc) {
        WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, g_regionOverride, sizeof(g_regionOverride), NULL, NULL);
        ++i;
      }
    } else if (lstrcmpW(arg, L"-timestep") == 0 || lstrcmpW(arg, L"-fixedtimestep") == 0) {
      // Deprecated — silently consume value arg if present
      if (lstrcmpW(arg, L"-timestep") == 0 && i + 1 < argc) ++i;
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] %ls is deprecated and ignored", arg);
    } else if (lstrcmpW(arg, L"-headless") == 0 || lstrcmpW(arg, L"-noovr") == 0) {
      // Deprecated — silently ignored (-server implies headless, -noovr is always on)
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] %ls is deprecated and ignored", arg);
    }
  }

  // -noovr is always applied (no OVR runtime needed)
  // -server implies headless (all servers are headless)
  g_isHeadless = g_isHeadless || g_isServer;
  if (g_isServer) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Server mode — headless + noovr applied");
  }

  // Auto-enable -noconsole on Wine/Linux
  if (!g_noConsole) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL) {
      g_noConsole = TRUE;
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Wine detected — defaulting to -noconsole");
    }
  }

  // Validate argument combinations
  if (g_isServer && g_isOffline) {
    FatalError("Arguments -server and -offline are mutually exclusive.", NULL);
  }

  // Store the game pointer globally for social feature access
  g_pGame = pGame;

  // Apply patches based on arguments.
  if (g_isOffline) {
    PatchEnableOffline();
  }

  if (g_isHeadless) {
    PatchEnableHeadless(pGame);
  }

  // If the windowed, server, or headless flags were provided, apply the windowed mode patch to not use a VR headset.
  if (g_isWindowed || g_isServer || g_isHeadless) {
    using namespace PatchAddresses;
    // Set windowed mode flag in game structure
    UINT64* windowedFlags = reinterpret_cast<UINT64*>(static_cast<CHAR*>(pGame) + GAME_WINDOWED_FLAGS_OFFSET);
    *windowedFlags |= 0x0100000;  // Enable windowed mode (spectator uses 0x2100000 for additional settings)
  }

  // Apply patches to force the game to load as a server.
  if (g_isServer) {
    PatchDisableServerRendering(pGame);
    PatchEnableServer();
    PatchDisableLoadingTips();
    PatchBypassOvrPlatform();
    PatchBlockOculusSDK();
    PatchDisableWwise();
    PatchLogServerProfile();

    // Replace CPrecisionSleep's QPC busy-wait with Wine-friendly Sleep().
    // Only for headless servers — clients need precise frame timing for VR rendering.
    if (g_isHeadless) {
      PatchServerFramePacing();
    }
  }

  // Initialize built-in modules (after CLI flags are known)
  uintptr_t base = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
  bool isServer = g_isServer != FALSE;
  BuiltinServerTiming::Init(base, isServer);
  BuiltinTokenAuth::Init(base, isServer);

  // Load external plugins from plugins/ subdirectory
  LoadPlugins();

  // Run the original method
  UINT64 result = EchoVR::PreprocessCommandLine(pGame);
  return result;
}
