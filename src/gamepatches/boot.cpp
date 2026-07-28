#include "boot.h"
#include "cli.h"
#include "config.h"
#include "crash_recovery.h"
#include "initialize.h"
#include "mode_patches.h"
#include "resource_override.h"
#include "plugin_loader.h"
#include "module_loader.h"
#include "ws_bridge.h"
#include "pnsrad_enabler.h"
#include "wave0_instrumentation.h"
#include "patch_addresses.h"
#include "common/globals.h"
#include "common/logging.h"
#include "common/echovr_functions.h"
#include "common/nevr_module_interface.h"

#include <cstdlib>
#include <shellapi.h>

/// <summary>
/// A detour hook for the game's command line pre-processing method, used to parse command line arguments.
/// </summary>
/// <param name="pGame">A pointer to the game instance.</param>
UINT64 PreprocessCommandLineHook(PVOID pGame) {
  // Deferred from Initialize() — file I/O deadlocks during DllMain loader lock.
  LoadEarlyConfig();
  InstallResourceOverride();

  // Early-detect -server before auth — full CLI parse happens below.
  // The token_auth MODULE needs to know server mode to skip the device-code flow.
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 0; i < argc; ++i) {
      if (lstrcmpW(argv[i], L"-server") == 0) {
        g_isServer = TRUE;
        g_noOvr = TRUE;  // -server implies -noovr unconditionally
        break;
      }
    }
    LocalFree(argv);
  }

  // Install server-mode fatal error handler BEFORE loading modules.
  // If a required module fails to load, module_loader.cpp calls FatalError,
  // which must route through ForceFatalExit rather than blocking on a modal
  // MessageBoxA. In client mode the handler is NOT installed — the user at the
  // screen should see the modal dialog so they know the game fatally failed.
  if (g_isServer) {
    InstallFatalErrorHandler();

    // Deferred fatal-condition checks — these conditions are detected in
    // Initialize()/LoadEarlyConfig() before g_isServer is known, so we check
    // them now that the fatal-error handler is installed.  If any fail, the
    // server dies immediately with the cause as the last log line rather than
    // limping along in a degraded state for hours.
    if (g_earlyConfigPtr == NULL) {
      ServerFatal("_local/config.json not found or unparseable — server requires configuration");
    }
    if (g_bootHookFailed) {
      ServerFatal("One or more boot hooks failed to install — server would be degraded");
    }
  }

  // Load modules. Order matters — dependencies must load first.
  {
    static NvrModuleContext moduleCtx = {};
    moduleCtx.base_addr = (uintptr_t)EchoVR::g_GameBaseAddress;
    moduleCtx.early_config = g_earlyConfigPtr;
    moduleCtx.flags = 0;
    if (g_isServer)   moduleCtx.flags |= NEVR_MODULE_HOST_IS_SERVER;
    if (g_isHeadless) moduleCtx.flags |= NEVR_MODULE_HOST_IS_HEADLESS;
    if (g_noOvr)      moduleCtx.flags |= NEVR_MODULE_HOST_IS_NOOVR;
    if (!g_isServer)  moduleCtx.flags |= NEVR_MODULE_HOST_IS_CLIENT;
    moduleCtx.log = (void (*)(int, const char*, ...))Log;
    moduleCtx.get_proc = ResolveModuleProc;
    SetModuleContext(&moduleCtx);

    // Platform compat — Schannel TLS hooks, CreateDirectory fixes, WinHTTP bridge.
    // Must load before any network-using code.
    LoadModule("platform_compat", &moduleCtx);

    // Token auth — device code authentication, JWT refresh.
    // Must load before ws_bridge (ws_bridge reads the JWT via get_proc).
    LoadModule("token_auth", &moduleCtx);

    // Register token_auth exports for cross-module access (ws_bridge uses these).
    HMODULE hTokenAuth = GetModuleHandleA("token_auth.dll");
    if (hTokenAuth) {
      void* getToken = (void*)GetProcAddress(hTokenAuth, "TokenAuth_GetToken");
      void* getDiscordId = (void*)GetProcAddress(hTokenAuth, "TokenAuth_GetDiscordId");
      if (getToken) RegisterModuleProc("TokenAuth_GetToken", getToken);
      if (getDiscordId) RegisterModuleProc("TokenAuth_GetDiscordId", getDiscordId);
    }

    // N92: ws_bridge is no longer a module. It is compiled into this DLL and
    // started below, after the CLI is parsed. The LoadModule call and the
    // RegisterModuleProc registrations are gone with it — config.cpp now calls
    // IsWebSocketBridgeActive()/GetWebSocketBridgePort() directly.
    //
    // Removing the LoadModule call is REQUIRED, not cosmetic: ws_bridge was on
    // the required-module list, so with the DLL gone the loader correctly
    // fail-fasts with "[FATAL] ws_bridge: Required module missing" and exit 1.
    // Measured, 2026-07-27.
  }

  // Parse command line arguments.
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  for (int i = 0; i < argc; ++i) {
    const LPWSTR arg = argv[i];

    if (lstrcmpW(arg, L"-server") == 0) {
      g_isServer = TRUE;
      g_isHeadless = TRUE;  // -server forces -headless unconditionally (never separable)
    } else if (lstrcmpW(arg, L"-offline") == 0) {
      g_isOffline = TRUE;
    } else if (lstrcmpW(arg, L"-noconsole") == 0) {
      g_noConsole = TRUE;
    } else if (lstrcmpW(arg, L"-windowed") == 0) {
      g_isWindowed = TRUE;
    } else if (lstrcmpW(arg, L"-noexitonerror") == 0) {
      g_exitOnError = FALSE;
    } else if (lstrcmpW(arg, L"-exitonerror") == 0) {
      // Deprecated — exit-on-error is now the default
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] -exitonerror is deprecated (now default)");
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
    } else if (lstrcmpW(arg, L"-allow-dbgcore") == 0) {
      g_allowDbgCore = TRUE;
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
      // -server now forces -headless unconditionally. Passing -headless or -noovr
      // separately is a configuration error — these are not separable from -server.
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] %ls is redundant — -server forces -headless unconditionally. "
          "Remove this flag from your command line.", arg);
      if (lstrcmpW(arg, L"-noovr") == 0) {
        g_noOvr = TRUE;
      }
    }
  }

  LocalFree(argv);

  // -server forces -headless unconditionally (set in the -server branch above).
  // The OR here is a safety net: if g_isHeadless was set to TRUE for any other
  // reason, -noovr is also applied. In practice, g_isHeadless is only set by -server.
  g_isHeadless = g_isHeadless || g_isServer;
  // -server implies -noovr unconditionally.
  g_noOvr = g_noOvr || g_isServer;
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

  // Detect dbgcore.dll in the game directory — prevents accidental hijack.
  // In the launcher path, BugSplat64.dll is loaded by echovr_game.dll. If a
  // leftover dbgcore.dll is also present in the game directory, the OS loader
  // resolves it during import resolution BEFORE the launcher can intervene,
  // causing a double-load condition. The -allow-dbgcore flag permits this
  // intentionally for the legacy injection path.
  {
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
      char* lastSlash = strrchr(exePath, '\\');
      if (!lastSlash) lastSlash = strrchr(exePath, '/');
      if (lastSlash) {
        *(lastSlash + 1) = '\0';
        char dbgcorePath[MAX_PATH];
        int written = snprintf(dbgcorePath, MAX_PATH, "%sdbgcore.dll", exePath);
        if (written > 0 && written < MAX_PATH) {
          if (GetFileAttributesA(dbgcorePath) != INVALID_FILE_ATTRIBUTES) {
            if (g_allowDbgCore) {
              Log(EchoVR::LogLevel::Info,
                  "[NEVR.PATCH] dbgcore.dll present in game directory — "
                  "legacy injection permitted (-allow-dbgcore)");
            } else {
              FatalError(
                  "dbgcore.dll detected in the game directory.\n\n"
                  "This file is a legacy DLL hijack artifact. If you are intentionally using "
                  "the legacy injection path, add -allow-dbgcore to your command line.\n\n"
                  "If you are using the NEVR launcher, remove dbgcore.dll from the game "
                  "directory and use BugSplat64.dll instead.",
                  "dbgcore.dll hijack detected");
            }
          }
        }
      }
    }
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

  // Force the game to load pnsrad.dll instead of pnsovr.dll.
  // Must run before the game's module loader starts.
  PnsradEnabler::Init((uintptr_t)EchoVR::g_GameBaseAddress);

  // Block Oculus Platform SDK on server/headless — client needs Oculus Platform services
  if (g_isServer || g_isHeadless) {
    PatchBypassOvrPlatform();
    PatchBlockOculusSDK();
  }

  // Apply patches to force the game to load as a server.
  if (g_isServer) {
    PatchEnableServer();
    PatchDisableLoadingTips();
    PatchDisableWwise();
    PatchLogServerProfile();

    // Server frame pacing is handled by the server-timing plugin.
    PatchServerFramePacing();
  }

  // N92: start the WebSocket bridge in-process. It used to be
  // modules/ws_bridge.dll, started from that module's NvrModuleInit. Folding it
  // into this DLL removes the second, divergent copy that had drifted apart from
  // the shipping one — session sharing lived in the copy that never ran, and the
  // fake-LoginSuccess path lived only in the one that did.
  //
  // Started here, before plugins, because config.cpp's service redirect needs the
  // bridge port and the game asks for redirects during PreprocessCommandLine.
  if (g_earlyConfigPtr) {
    CHAR* socketUri = EchoVR::JsonValueAsString(
        (EchoVR::Json*)g_earlyConfigPtr, (CHAR*)"nevr_socket_uri", NULL, false);
    if (socketUri && socketUri[0] != '\0') {
      SetWebSocketBridgeTarget(socketUri);
      InstallWebSocketBridge();
    } else {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.WS] no nevr_socket_uri in early config — bridge NOT started; the game "
          "will talk to services directly and login injection cannot fire");
    }
  }

  // Load external plugins from plugins/ subdirectory
  LoadPlugins();

  // Crash frames in modules/plugins are unattributable without this (N85).
  RefreshModuleCache();
  ResolveShutdownDependencies();  // N62

  // Run the original method
  UINT64 result = EchoVR::PreprocessCommandLine(pGame);
  return result;
}
