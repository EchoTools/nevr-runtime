#include "runtime/lifecycle/boot.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/service_config.h"  // NevrCfgGetFlat (N133 S4a: config.yaml reads)
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/initialize.h"
#include "runtime/patch/mode_patches.h"
#include "runtime/patch/resource_override.h"
#include "runtime/patch/asset_cdn.h"
#include "runtime/ext/plugin_loader.h"
#include "extension/module_interface.h"

// Statically-linked module entry points (2026-08-02: folded from separate
// DLLs into BugSplat64.dll). Each module's symbols are prefixed to avoid
// collisions — both used to export NvrModuleInit/NvrModuleApiVersion/etc.
// as separate DLLs with their own symbol tables.
extern "C" {
// platform_compat
int platform_compat_Init(const NvrModuleContext* ctx);
uint32_t platform_compat_ApiVersion(void);
void platform_compat_Shutdown(void);
// token_auth
int token_auth_Init(const NvrModuleContext* ctx);
uint32_t token_auth_ApiVersion(void);
void token_auth_Shutdown(void);
const char* TokenAuth_GetToken(void);
uint64_t TokenAuth_GetDiscordId(void);
const char* TokenAuth_GetUsername(void);
}
#include "runtime/ext/module_loader.h"
#include "runtime/compat/ws_bridge.h"
#include "runtime/patch/pnsrad_enabler.h"
#include "runtime/patch/binary_bug_fixes.h"
#include "runtime/hook/addresses.h"
#include "core/globals.h"
#include "core/logging.h"
#include "abi/echovr_functions.h"
#include "extension/module_interface.h"

#include <cstdlib>
#include <atomic>
#include <shellapi.h>

namespace {

// N146: only this minimal state is needed before the original command-line
// preprocessing.  In particular, do not start modules, plugins, file I/O, or
// the bridge here: client D3D initialization is still in the original call.
// -server is needed for the config loader's fail-loud policy; -config-path is
// needed by LoadLocalConfigHook while the original is running.
void PreflightRuntimeBootstrap() {
  static bool s_done = false;
  if (s_done) return;
  s_done = true;

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return;

  for (int i = 0; i < argc; ++i) {
    const LPWSTR arg = argv[i];
    if (lstrcmpW(arg, L"-server") == 0) {
      g_isServer = TRUE;
      g_noOvr = TRUE;  // -server implies -noovr unconditionally
    } else if ((lstrcmpW(arg, L"-config") == 0 || lstrcmpW(arg, L"-config-path") == 0) && i + 1 < argc) {
      WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_customConfigPath, MAX_PATH, NULL, NULL);
    }
  }
  LocalFree(argv);
}

}  // namespace

/// <summary>
/// A detour hook for the game's command line pre-processing method, used to parse command line arguments.
/// </summary>
/// <param name="pGame">A pointer to the game instance.</param>
UINT64 PreprocessCommandLineHook(PVOID pGame) {
  // N146: the first original call initializes the client D3D path.  It can also
  // be the ONLY call, so waiting for a second invocation leaves the bridge and
  // all deferred runtime setup permanently disabled.  Store the game instance
  // before D3D starts so its device hook can complete setup immediately after
  // creation. A dedicated server retains the established second-preprocess
  // boundary because it has no graphics-device call to rendezvous on.
  static int s_callCount = 0;
  ++s_callCount;
  g_pGame = pGame;
  PreflightRuntimeBootstrap();
  UINT64 result = EchoVR::PreprocessCommandLine(pGame);
  if (g_isServer && s_callCount >= 2) {
    RunDeferredRuntimeBootstrap(pGame, "Preprocess second-call server fallback");
  }
  return result;
}

void RunDeferredRuntimeBootstrap(PVOID pGame, const char* trigger) {
  // The game can reach this through a post-device graphics hook and, for a
  // dedicated server, through the second-preprocess fallback above. Mark
  // initialization started before any work so a re-entrant engine call cannot
  // initialise modules/listeners twice.
  static std::atomic_bool s_oneTimeSetupStarted{false};
  bool expected = false;
  if (!s_oneTimeSetupStarted.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
    return;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.BOOT] runtime bootstrap trigger=%s",
      trigger ? trigger : "(unknown)");

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
    // N133 S5: modules read config.yaml through the SAME accessor the runtime uses,
    // so a module never parses the game JSON and never sees a value the runtime
    // wouldn't. NevrCfgGetFlat's fail-loud is mode-aware (server=fatal/client=warn),
    // which a module inherits by calling through this pointer.
    moduleCtx.config_get = &NevrCfgGetFlat;
    SetModuleContext(&moduleCtx);

    // Platform compat — Schannel TLS hooks, CreateDirectory fixes, WinHTTP bridge.
    // Must load before any network-using code. Statically linked (2026-08-02).
    {
      uint32_t apiVer = platform_compat_ApiVersion();
      if (!NvrModuleApiVersionSupported(apiVer)) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.MODULE] platform_compat: API v%u exceeds host v%u — refusing",
            apiVer, static_cast<uint32_t>(NEVR_MODULE_API_VERSION));
        FatalError("Module API version unsupported", "platform_compat");
      }
      if (platform_compat_Init(&moduleCtx) != 0) {
        FatalError("Module init failed", "platform_compat");
      }
      RegisterStaticModule("platform_compat", apiVer, nullptr, nullptr, platform_compat_Shutdown);
    }

    // Token auth — device code authentication, JWT refresh.
    // Must load before ws_bridge (ws_bridge reads the JWT via get_proc).
    // Statically linked (2026-08-02).
    {
      uint32_t apiVer = token_auth_ApiVersion();
      if (!NvrModuleApiVersionSupported(apiVer)) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.MODULE] token_auth: API v%u exceeds host v%u — refusing",
            apiVer, static_cast<uint32_t>(NEVR_MODULE_API_VERSION));
        FatalError("Module API version unsupported", "token_auth");
      }
      if (token_auth_Init(&moduleCtx) != 0) {
        FatalError("Module init failed", "token_auth");
      }
      RegisterStaticModule("token_auth", apiVer, nullptr, nullptr, token_auth_Shutdown);

      // Register token_auth exports for cross-module access (ws_bridge reads these
      // via ResolveModuleProc). Static link means no GetProcAddress — call directly.
      RegisterModuleProc("TokenAuth_GetToken", (void*)TokenAuth_GetToken);
      RegisterModuleProc("TokenAuth_GetDiscordId", (void*)TokenAuth_GetDiscordId);
      RegisterModuleProc("TokenAuth_GetUsername", (void*)TokenAuth_GetUsername);
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
      g_noOvr = TRUE;  // -windowed implies -noovr (no VR headset under Wine)
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
    } else if (lstrcmpW(arg, L"-headless") == 0) {
      // N99: this branch used to call -headless "redundant" and tell the
      // operator to remove it. That was false and it cost a real regression —
      // a unit believed the message, removed the flag, and a window opened on
      // the owner's screen. -headless is a NATIVE echovr.exe token; the game's
      // own arg handler applies pGame+0x1D4 &= 0xFFFEFEFE only when it is
      // present. NEVR now applies that same mask for -server
      // (PatchEnableHeadless), and AND-masking is idempotent, so the token is
      // harmless either way. Say only what was measured, and instruct the
      // operator to remove nothing.
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] -headless: native echovr.exe flag; -server applies the same "
          "engine-flags mask, so this token is a harmless no-op here.");
    } else if (lstrcmpW(arg, L"-noovr") == 0) {
      // Native token, and the game handles it itself: a single flag write,
      // orq $0x8000000, 0x7ae0(%rdi) at 0x1401168BA. NOT pGame+0x178 — that is
      // the CMainArgs POINTER, and the claim that it was a flag field is what
      // kept this open in N99 for a week. Closed not-a-defect in N113.
      //
      // g_noOvr is not "only a global": it drives NEVR_MODULE_HOST_IS_NOOVR
      // (:75) and the login platform code 5-vs-2 (compat/ws_bridge.cpp:485,544),
      // which is the behaviour -noovr exists to produce. The game's own
      // -noovr-requires--spectatorstream assert is bypassed by
      // PatchNoOvrRequiresSpectatorStream, inert unless the token is present.
      g_noOvr = TRUE;
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

    // (PatchServerFramePacing removed 2026-07-29 — N113. It blind-wrote 0xC3 to
    // CPrecisionSleep::BusyWait with no address validation and no original-byte
    // save, duplicating the canonical patch in patch/binary_bug_fixes.cpp which
    // does both. Two writers to an address whose ORIGINAL byte a shutdown
    // restore depends on; safe only because Init happened to run first.)
  }

  // N131: cosmetics are client-only — a headless server has nothing to render and
  // must not open the CDN connection (it opens ServerDB + login only). AssetCDN
  // was called UNCONDITIONALLY from initialize.cpp:364, which runs before the CLI
  // is parsed, so g_isServer was still FALSE there and a server fetched tints it
  // never draws. Moved here, post-CLI-parse where g_isServer is known, gated on
  // client — the same deferral InstallResourceOverride uses (boot.cpp:29). The
  // loadout SAVE/CURRENT protocol in gameserver.cpp is independent of this hook,
  // so gating it off on a server does not affect loadout handling.
  if (!g_isServer) {
    AssetCDN::Initialize();
  }

  // N92: start the WebSocket bridge in-process. It used to be
  // modules/ws_bridge.dll, started from that module's NvrModuleInit. Folding it
  // into this DLL removes the second, divergent copy that had drifted apart from
  // the shipping one — session sharing lived in the copy that never ran, and the
  // fake-LoginSuccess path lived only in the one that did.
  //
  // Started here, before plugins, because config.cpp's service redirect needs the
  // bridge port and the game asks for redirects during PreprocessCommandLine.
  // N133 S4a: the bridge target (nevr_socket_uri) now comes from config.yaml
  // services.socket_uri via nevr_config, not the game JSON. No g_earlyConfigPtr
  // guard here — the value no longer lives in the early game config; the bridge
  // starts iff socket_uri is configured (absent -> no bridge, unchanged). First
  // NevrCfg() access happens here, after the CLI loop (so -config-path is
  // honoured), after g_isServer/InstallFatalErrorHandler — a bad config.yaml or
  // an unset required secret fails loud at this point in server mode.
  {
    const char* socketUri = NevrCfgGetFlat("nevr_socket_uri");
    if (socketUri && socketUri[0] != '\0') {
      SetWebSocketBridgeTarget(socketUri);
      InstallWebSocketBridge();
    } else {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.WS] no services.socket_uri in config.yaml — bridge NOT started; the game "
          "will talk to services directly and login injection cannot fire");
    }
  }

  // Load external plugins from plugins/ subdirectory
  LoadPlugins();

  // Crash frames in modules/plugins are unattributable without this (N85).
  RefreshModuleCache();
  ResolveShutdownDependencies();  // N62

  // N87: re-arm the console ctrl handler so CTRL+C works in client mode.
  // Our handler is installed early (behind the game's) during Initialize().
  // RearmConsoleCtrlHandler re-registers it at the front so it fires first.
  // Previously this was only called from the server path (GameServerLib::Terminate).
  RearmConsoleCtrlHandler();

  Log(EchoVR::LogLevel::Info,
      "[NEVR.BOOT] runtime bootstrap complete early_config=%d bridge=%d port=%u",
      g_earlyConfigPtr != nullptr ? 1 : 0, IsWebSocketBridgeActive() ? 1 : 0,
      static_cast<unsigned>(GetWebSocketBridgePort()));

}
