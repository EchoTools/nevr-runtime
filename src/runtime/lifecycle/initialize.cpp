#include "runtime/lifecycle/initialize.h"
#include "runtime/lifecycle/game_image_guard.h"

#include <cstring>
#include <vector>

#include "runtime/log/boot_log_tee.h"
#include "runtime/lifecycle/boot.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/veh_policy.h"
#include "runtime/hook/patching.h"
#include "runtime/patch/mode_patches.h"
// platform_compat lives in src/modules/platform-compat (loaded in boot.cpp).
#include "runtime/patch/resource_override.h"
#include "runtime/lifecycle/state_machine.h"
#include "runtime/compat/ws_bridge.h"
#include "runtime/server/gameserver.h"

#include "runtime/patch/broadcaster_guard.h"
#include "runtime/log/builtin_filter.h"
#include "runtime/hook/dll_load_hook.h"
#include "runtime/hook/export_tracer.h"
#include "runtime/patch/headless_graphics.h"
#include "core/globals.h"
#include "core/hooking.h"
#include "core/logging.h"
#include "abi/echovr_functions.h"
#include "runtime/hook/addresses.h"
#include "runtime/patch/binary_bug_fixes.h"
#include "runtime/patch/xpid_patch.h"
#include "runtime/patch/pnsrad_enabler.h"
#include "runtime/patch/mic_provider.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/patch/early_quit_lockout.h"
#include "runtime/patch/social_facade.h"

#include <windows.h>

// ============================================================================
// Internal state
// ============================================================================

static BOOL g_initialized = FALSE;
bool g_bootHookFailed = false;

HWND g_hWindow = NULL;

// ============================================================================
// SetWindowTextA hook — captures window handle
// ============================================================================

static BOOL SetWindowTextAHook(HWND hWnd, LPCSTR lpString) {
  g_hWindow = hWnd;
  return (BOOL)EchoVR::SetWindowTextA_(hWnd, lpString);
}

// ============================================================================
// GameServerLib factory — provides IServerLib to the game via CSysDLL_GetSymbol
// ============================================================================

static EchoVR::IServerLib* g_ServerLib = nullptr;

static EchoVR::IServerLib* ServerLibFactory() {
    if (!g_ServerLib) {
        g_ServerLib = new GameServerLib();
        BootLogTee::TeeFprintf("[NEVR.GAMESERVER] ServerLib() created obj=%p\n", static_cast<void*>(g_ServerLib));
    }
    return g_ServerLib;
}

// ============================================================================
// CSysDLL_GetSymbol hook — intercepts game's internal DLL symbol resolution
// to provide our GameServerLib via ServerLibFactory
// ============================================================================
// CSysDLL_GetSymbol @ 0x1400eaef0 — the game's own GetProcAddress wrapper.
// Used by CNSLobby_LoadServerSupport to resolve "ServerLib" from pnsradgameserver.dll.

typedef void* (*CSysDLL_GetSymbol_fn)(void* dll_handle, const char* symbol_name);
static CSysDLL_GetSymbol_fn g_original_GetSymbol = nullptr;

// GH#15 / docs/design/2026-09-21-mic-provider-voip-fix.md: pnsrad.dll's
// MicAvailable/MicCreate/MicDetected/MicRead are identical-code-folded onto
// one address, and MicDestroy/MicStart/MicStop onto a second — a MinHook
// detour on either address cannot distinguish which export name the game
// meant to resolve. This is the SAME address-collision trap already
// handled below for RadPluginShutdown (N128): 0x1400eaef0 is the game's
// one symbol-resolution function, and it's what NRadEngine::CPlatformService::
// MicRead (echovr.exe 0x14060cad0) calls to resolve "MicRead" etc. on a
// provider handle. So the mic exports are intercepted HERE, by name, against
// pnsrad.dll's module handle specifically — never by hooking pnsrad's own
// stub bodies.
static void* MicProviderSymbolOverride(void* dll_handle, const char* symbol_name) {
  if (!symbol_name) return nullptr;
  uintptr_t pnsradBase = PnsradEnabler::GetModuleBase();
  if (pnsradBase == 0 || reinterpret_cast<uintptr_t>(dll_handle) != pnsradBase) return nullptr;

  if (strcmp(symbol_name, "MicAvailable") == 0) return reinterpret_cast<void*>(&MicProvider::MicAvailable);
  if (strcmp(symbol_name, "MicCreate") == 0) return reinterpret_cast<void*>(&MicProvider::MicCreate);
  if (strcmp(symbol_name, "MicDetected") == 0) return reinterpret_cast<void*>(&MicProvider::MicDetected);
  if (strcmp(symbol_name, "MicRead") == 0) return reinterpret_cast<void*>(&MicProvider::MicRead);
  if (strcmp(symbol_name, "MicStart") == 0) return reinterpret_cast<void*>(&MicProvider::MicStart);
  if (strcmp(symbol_name, "MicStop") == 0) return reinterpret_cast<void*>(&MicProvider::MicStop);
  if (strcmp(symbol_name, "MicDestroy") == 0) return reinterpret_cast<void*>(&MicProvider::MicDestroy);
  // MicBufferSize/MicCaptureSize/MicSampleRate are NOT overridden: pnsrad's
  // own (unmodified) answers are already correct (24000/2400/48000) and
  // nothing downstream needs them to change.
  return nullptr;
}

static void* CSysDLL_GetSymbolHook(void* dll_handle, const char* symbol_name) {
  // The export tracer (#20; off unless -traceexports names a platform DLL) wraps what the game is handed for a
  // symbol of a selected DLL, so the mic provider's overrides are traced as well as pnsrad's own exports.
  static const bool tracerConfigured = (ExportTracer::ConfigureFromCommandLine(), true);
  static_cast<void>(tracerConfigured);
  const auto traced = [dll_handle, symbol_name](void* resolved) {
    return ExportTracer::WrapSymbol(dll_handle, symbol_name, resolved);
  };
  if (symbol_name && strcmp(symbol_name, "ServerLib") == 0) {
    static bool logged = false;
    if (!logged) {
        BootLogTee::TeeFprintf("[NEVR.GAMESERVER] serverlib symbol resolved -> gamepatches factory\n");
        logged = true;
    }
    return reinterpret_cast<void*>(&ServerLibFactory);
  }
  if (void* micFn = MicProviderSymbolOverride(dll_handle, symbol_name)) {
    static bool logged = false;
    if (!logged) {
      BootLogTee::TeeFprintf("[NEVR.MIC] pnsrad mic export(s) resolved -> WASAPI provider\n");
      logged = true;
    }
    return traced(micFn);
  }
  // Platform DLLs (pnsdemo/pnsovr) crash in RadPluginShutdown on a server (freed
  // memory). They are recognised by the "Users" export they all define. The game's
  // unload path (0x14105ae30) null-checks the resolved symbol before calling it, so
  // answering null skips the call and teardown carries on. (A second detour on
  // this same address would not install under MinHook, #93/#94; ending the
  // process with exit(0) instead is suppressed by ExitProcessHook in server
  // mode.)
  if (g_isServer && symbol_name && strcmp(symbol_name, "RadPluginShutdown") == 0 &&
      g_original_GetSymbol(dll_handle, "Users") != nullptr) {
    static bool logged = false;
    if (!logged) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] RadPluginShutdown of a platform DLL skipped (server)");
      logged = true;
    }
    return nullptr;
  }
  void* result = g_original_GetSymbol(dll_handle, symbol_name);

  // XInput stubs (keep these)
  if (symbol_name && result != nullptr) {
    static auto xinput_get_state = +[](uint32_t, void*) -> uint32_t { return 0x48F; };
    static auto xinput_set_state = +[](uint32_t, void*) -> uint32_t { return 0x48F; };
    static auto xinput_get_caps  = +[](uint32_t, uint32_t, void*) -> uint32_t { return 0x48F; };

    if (strcmp(symbol_name, "XInputGetState") == 0) {
      static bool logged = false;
      if (!logged) { BootLogTee::TeeFprintf("[NEVR.PATCH] xinput stub name=XInputGetState state=not_connected\n"); logged = true; }
      return reinterpret_cast<void*>(xinput_get_state);
    }
    if (strcmp(symbol_name, "XInputSetState") == 0) return reinterpret_cast<void*>(xinput_set_state);
    if (strcmp(symbol_name, "XInputGetCapabilities") == 0) return reinterpret_cast<void*>(xinput_get_caps);
  }

  return traced(result);
}

// ============================================================================
// CSysDLL_Load (CModule load wrapper) hook — completes the gameserver migration
// ============================================================================
// 0x14105aa70 — the game's DLL load wrapper. LoadServerSupport (0x14060bb70)
// calls it to load "pnsradgameserver" BEFORE resolving "ServerLib" via
// CSysDLL_GetSymbol. Now that the gameserver lives in BugSplat64.dll and the
// external file is gone, that load fails and the game bails with "Unable to
// load server library" — before the GetSymbol hook above can supply the factory.
//
// Fix: when the REAL load of pnsradgameserver fails, return a benign pinned
// HMODULE (kernel32) so LoadServerSupport proceeds to GetSymbol("ServerLib"),
// which is redirected to ServerLibFactory above. Verified against the binary:
// on the success path the handle is only stored (this+0x30) and passed to
// GetSymbol — never freed or dereferenced as a CModule. kernel32 exports
// neither "ServerLib" nor "RadPluginShutdown" and tolerates the teardown
// FreeLibrary cleanly. Calling the original first keeps the real-file path
// (and its RadPlugin bootstrap) intact if the DLL is ever present again.

typedef void* (*CSysDLL_Load_fn)(void* name_buf, void* plugin_ctx);
static CSysDLL_Load_fn g_original_LoadModule = nullptr;

// Case-insensitive substring check against the ANSI name string at name_buf[0].
// needle must be lowercase. Bounded scan to avoid a runaway read.
static bool LoadNameContains(const void* name_buf, const char* needle) {
  if (!name_buf) return false;
  const char* hay = reinterpret_cast<const char*>(name_buf);
  char low[512];
  size_t i = 0;
  for (; i < sizeof(low) - 1 && hay[i] != '\0'; ++i) {
    char c = hay[i];
    low[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  low[i] = '\0';
  return strstr(low, needle) != nullptr;
}

static void* CSysDLL_LoadHook(void* name_buf, void* plugin_ctx) {
  void* real = g_original_LoadModule(name_buf, plugin_ctx);
  if (real != nullptr) return real;  // normal load (file present) or non-target DLL

  // Load failed. If this is the migrated server library, hand back a benign
  // pinned handle so the GetSymbol("ServerLib") hook can supply the factory.
  if (LoadNameContains(name_buf, "pnsradgameserver")) {
    static HMODULE s_fakeServerLibModule = LoadLibraryA("kernel32.dll");
    if (s_fakeServerLibModule) {
      static bool logged = false;
      if (!logged) {
        BootLogTee::TeeFprintf("[NEVR.GAMESERVER] pnsradgameserver load redirected to in-process "
                        "ServerLib factory (DLL eliminated, code lives in BugSplat64)\n");
        logged = true;
      }
      return reinterpret_cast<void*>(s_fakeServerLibModule);
    }
  }
  return nullptr;
}

// ============================================================================
// Game version verification
// ============================================================================

// ============================================================================
// Cross-DLL exports (called by gameserver.dll via GetProcAddress)
// ============================================================================

extern "C" __declspec(dllexport) void NEVR_ScheduleReturnToLobby() {
  if (g_pGame) EchoVR::NetGameScheduleReturnToLobby(g_pGame);
}

extern "C" __declspec(dllexport) void NEVR_GetUPnPConfig(NevRUPnPConfig* out) {
  if (!out) return;
  out->enabled = g_upnpEnabled;
  out->port    = g_upnpPort;
  memcpy(out->internalIp, g_internalIpOverride, sizeof(out->internalIp));
  memcpy(out->externalIp, g_externalIpOverride, sizeof(out->externalIp));
}

// ============================================================================
// Boot hook results
// ============================================================================

// Whether the process can do its job without a given boot hook. A required hook
// that does not install sets g_bootHookFailed, and boot.cpp refuses to start a
// server with that flag set rather than run it degraded; a client only reports
// ok=false in the final boot line. An optional hook that does not install is
// recorded and boot continues.
enum class BootHookRequirement { kRequired, kOptional };

// Issue #42: every boot hook's install result goes through here, so no call site
// can drop it. PatchDetour has already logged the MinHook reason on failure; this
// adds what the failure means for the boot. TeeFprintf, not Log(): this runs
// under the DllMain loader lock (see the N36 note in `just verify`).
static void NoteBootHookResult(BOOL installed, const char* name, BootHookRequirement requirement) {
  if (installed) return;
  if (requirement == BootHookRequirement::kRequired) {
    g_bootHookFailed = true;
    BootLogTee::TeeFprintf(
        "[NEVR.PATCH] required boot hook not installed name=%s; a server will refuse to start, a client "
        "continues without it\n",
        name);
  } else {
    BootLogTee::TeeFprintf("[NEVR.PATCH] optional boot hook not installed name=%s; boot continues\n", name);
  }
}

// The boot sequence's only way to detour a game function: the result cannot be
// discarded, and each call site has to state whether the hook is required.
template <typename T>
static void InstallBootDetour(T* ppPointer, PVOID pDetour, const char* name, BootHookRequirement requirement) {
  NoteBootHookResult(PatchDetour(ppPointer, pDetour, name), name, requirement);
}

// ============================================================================
// Main initialization
// ============================================================================

static VOID InitializeAfterGameImageGuard() {
  if (g_initialized) return;
  g_initialized = true;

  // Open the boot log BEFORE the first fprintf — all boot messages tee to both
  // stderr and nevr-boot.jsonl so early failures leave a record.  Uses only
  // kernel32 (CreateFileA / WriteFile), safe under the loader lock on a
  // static-CRT build (see boot_log_tee.h DESIGN DECISION and N43).
  BootLogTee::Init();

  BootLogTee::TeeFprintf("[NEVR.PATCH] Initializing v%s base=%p\n", PROJECT_VERSION, EchoVR::g_GameBaseAddress);
  EchoVR::InitializeFunctionPointers();
  BootLogTee::TeeFprintf("[NEVR.PATCH] function pointers resolved\n");

  BootLogTee::TeeFprintf("[NEVR.BOOT] initializing hooking engine...\n");
  if (!Hooking::Initialize()) {
    BootLogTee::TeeFprintf("[NEVR.PATCH] FATAL hooking init failed\n");
    g_bootHookFailed = true;
    BootLogTee::Close();  // the boot phase ends here too: later callers must use Log(), not the tee
    return;
  }
  BootLogTee::TeeFprintf("[NEVR.PATCH] minhook initialized\n");

  // Observe the platform Social factory result on every run. The hook preserves
  // a real provider object and substitutes the façade only for a null one, unless
  // `social.facade: false` turns the substitution off.
  SocialFacade::Install(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress));

  // The early quit lockout: the shipped client cannot show it from any game service message
  // (early_quit_lockout.h).
  EarlyQuitLockout::Install(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress));

  // --- DLL load interceptor (patch DLLs as they load) ---
  BootLogTee::TeeFprintf("[NEVR.BOOT] installing DLL load hooks...\n");
  DllLoadHook::Install();
  BootLogTee::TeeFprintf("[NEVR.PATCH] dll load hooks installed\n");

  // --- Headless graphics stubs (DXGI/D3D11 interception) ---
  // Register callbacks now so they fire when the game loads dxgi.dll/d3d11.dll.
  // g_isHeadless may not be set yet (CLI not parsed), but the hook checks it at
  // call time — if the game is running in headless mode the stubs activate,
  // otherwise they pass through to real DirectX.
  BootLogTee::TeeFprintf("[NEVR.BOOT] registering headless graphics hooks...\n");
  InstallHeadlessGraphicsHooks();
  BootLogTee::TeeFprintf("[NEVR.HEADLESS] graphics hooks registered\n");

  // N59: re-wire PatchDscProvider — the call site was lost when N43's
  // Initialize() rewrite merged over N41's include+call (a6bb57d).
  // Without this, the 5-site PSN→DSC + ???→DSC string-table rewrite
  // never executes, and the game paths that do not go through the
  // GetProviderPrefix detour below (GetUserIDString) format PSN-/???-
  // instead of DSC- (RULINGS.md 2026-07-20 login-prefix).
  BootLogTee::TeeFprintf("[NEVR.BOOT] patching DSC provider strings...\n");
  PatchDscProvider();
  BootLogTee::TeeFprintf("[NEVR.BOOT] detouring GetProviderPrefix → OVR-ORG...\n");
  PatchProviderPrefixOvrOrg();

  BootLogTee::TeeFprintf("[NEVR.BOOT] installing CSysDLL hooks...\n");
  {
      void* sym_target = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress) + (0x1400eaef0 - 0x140000000));
      // Prologue of CSysDLL_GetSymbol (echovr.exe 0x1400eaef0: sub rsp,0xA8; test rdx,rdx).
      static const unsigned char kGetSymbolPrologue[8] = {0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x48};
      if (memcmp(sym_target, kGetSymbolPrologue, sizeof(kGetSymbolPrologue)) != 0) {
        BootLogTee::TeeFprintf(
            "[NEVR.PATCH] hook skipped name=CSysDLL_GetSymbol va=0x1400eaef0 reason=prologue_mismatch\n");
        g_bootHookFailed = true;
      } else if (MH_CreateHook(sym_target, reinterpret_cast<void*>(&CSysDLL_GetSymbolHook),
                     reinterpret_cast<void**>(&g_original_GetSymbol)) == MH_OK &&
                 MH_EnableHook(sym_target) == MH_OK) {
        BootLogTee::TeeFprintf("[NEVR.PATCH] hooked name=CSysDLL_GetSymbol\n");
      } else {
        BootLogTee::TeeFprintf("[NEVR.PATCH] hook failed name=CSysDLL_GetSymbol\n");
        g_bootHookFailed = true;
      }
  }

  {
      void* load_target = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress) + (0x14105aa70 - 0x140000000));
      static const unsigned char kLoadModulePrologue[8] = {0x40, 0x53, 0x48, 0x81, 0xEC, 0x20, 0x04, 0x00};
      if (memcmp(load_target, kLoadModulePrologue, sizeof(kLoadModulePrologue)) != 0) {
        BootLogTee::TeeFprintf("[NEVR.PATCH] hook skipped name=CSysDLL_Load va=0x14105aa70 reason=prologue_mismatch\n");
      } else if (MH_CreateHook(load_target, reinterpret_cast<void*>(&CSysDLL_LoadHook),
                     reinterpret_cast<void**>(&g_original_LoadModule)) == MH_OK &&
                 MH_EnableHook(load_target) == MH_OK) {
        BootLogTee::TeeFprintf("[NEVR.PATCH] hooked name=CSysDLL_Load effect=pnsradgameserver_to_inprocess_serverlib\n");
      } else {
        BootLogTee::TeeFprintf("[NEVR.PATCH] hook failed name=CSysDLL_Load\n");
        g_bootHookFailed = true;
      }
  }

  // --- Broadcaster dispatch guard ---
  BootLogTee::TeeFprintf("[NEVR.BOOT] installing broadcaster guard...\n");
  BroadcasterGuard::Install(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress));
  // Truthful outcome: Install() is an empty placeholder (broadcaster_guard.cpp).
  // The previous line here read "broadcaster guard installed" — a log line
  // asserting a fact that is false in the source it describes. An operator (or
  // an agent) reading it would conclude a dispatch guard exists. None does.
  BootLogTee::TeeFprintf("[NEVR.PATCH] broadcaster guard: no-op placeholder, nothing installed\n");

  // --- Log filter (hooks CLog::PrintfImpl to capture/filter/file game output) ---
  // g_isServer not set yet (CLI not parsed); pass false — log filter works regardless
  BootLogTee::TeeFprintf("[NEVR.BOOT] initializing log filter...\n");
  BuiltinLogFilter::Init(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress), false);
  BootLogTee::TeeFprintf("[NEVR.PATCH] log filter installed\n");

  // --- Game function hooks ---
  BootLogTee::TeeFprintf("[NEVR.BOOT] installing game function hooks...\n");
  BOOL r1 = Hooking::Attach(reinterpret_cast<PVOID*>(&EchoVR::BuildCmdLineSyntaxDefinitions),
                             reinterpret_cast<PVOID>(BuildCmdLineSyntaxDefinitionsHook));
  BootLogTee::TeeFprintf("[NEVR.PATCH] hook name=BuildCmdLineSyntaxDefinitions result=%s\n", r1 ? "OK" : "FAILED");
  NoteBootHookResult(r1, "BuildCmdLineSyntaxDefinitions", BootHookRequirement::kRequired);
  BOOL r2 = Hooking::Attach(reinterpret_cast<PVOID*>(&EchoVR::PreprocessCommandLine),
                             reinterpret_cast<PVOID>(PreprocessCommandLineHook));
  BootLogTee::TeeFprintf("[NEVR.PATCH] hook name=PreprocessCommandLine result=%s\n", r2 ? "OK" : "FAILED");
  NoteBootHookResult(r2, "PreprocessCommandLine", BootHookRequirement::kRequired);
  // Required: on a server it turns NoNetwork/LoadFailed back into a usable
  // state, ends the process when a session ends, and is the shutdown-request
  // check when the frame pacer is not running (state_machine.cpp).
  InstallBootDetour(&EchoVR::NetGameSwitchState, reinterpret_cast<PVOID>(NetGameSwitchStateHook),
                    "EchoVR::NetGameSwitchState", BootHookRequirement::kRequired);
  // Required: sets g_localConfig, which HttpConnectHook needs before it
  // redirects anything, and supplies the built-in game config when no
  // config.json exists (config.cpp LoadLocalConfigHook).
  InstallBootDetour(&EchoVR::LoadLocalConfig, reinterpret_cast<PVOID>(LoadLocalConfigHook), "EchoVR::LoadLocalConfig",
                    BootHookRequirement::kRequired);
  // Required: without it the configured arena rule overrides (round time,
  // celebration time, mercy score) are silently not applied.
  InstallBootDetour(&EchoVR::CJsonGetFloat, reinterpret_cast<PVOID>(CJsonGetFloatHook), "EchoVR::CJsonGetFloat",
                    BootHookRequirement::kRequired);
  // Required: redirects the game's HTTP(S) service endpoints.
  InstallBootDetour(&EchoVR::HttpConnect, reinterpret_cast<PVOID>(HttpConnectHook), "EchoVR::HttpConnect",
                    BootHookRequirement::kRequired);
  // Optional: the hook only records the window handle in g_hWindow, and nothing
  // in the runtime reads g_hWindow.
  InstallBootDetour(&EchoVR::SetWindowTextA_, reinterpret_cast<PVOID>(SetWindowTextAHook), "EchoVR::SetWindowTextA_",
                    BootHookRequirement::kOptional);
  // Required: rewrites the game's service URLs (readyatdawn.com and ws/wss
  // hosts) to the configured services and supplies early-config overrides
  // (config.cpp JsonValueAsStringHook).
  InstallBootDetour(&EchoVR::JsonValueAsString, reinterpret_cast<PVOID>(JsonValueAsStringHook),
                    "EchoVR::JsonValueAsString", BootHookRequirement::kRequired);
  BootLogTee::TeeFprintf("[NEVR.PATCH] game hooks installed\n");
  // --- Platform compatibility hooks ---
  // InstallTLSHook() not needed — WebSocket bridge handles TLS via ixwebsocket.
  // The game's HTTP/REST calls go through the system MSXML6 XMLHTTP object over Schannel;
  // platform_compat only logs its creation (InstallMsxml6PassThroughHook).
  // WebSocket bridge (InstallWebSocketBridge) is started in PreprocessCommandLineHook
  // after config is loaded — it needs the wss:// URI from config.json.
  BootLogTee::TeeFprintf("[NEVR.PATCH] tls deferred=ws_bridge stage=boot\n");
  BootLogTee::TeeFprintf("[NEVR.BOOT] installing crash recovery hooks...\n");
  InstallCrashRecoveryHooks();
  BootLogTee::TeeFprintf("[NEVR.CRASH] crash recovery hooks installed\n");
  // CreateDirectory + MSXML6 pass-through hooks live in the platform_compat module (loaded in boot.cpp)
  BootLogTee::TeeFprintf("[NEVR.PATCH] platform hooks deferred=platform_compat_module\n");

  // --- Server crash recovery hooks ---
  BootLogTee::TeeFprintf("[NEVR.BOOT] installing server crash-recovery hooks...\n");
  InstallGameMainHook();
  InstallEntityHooks();
  InstallBugSplatHook();
  InstallGameSpaceHook();
  BootLogTee::TeeFprintf("[NEVR.PATCH] server crash-recovery hooks installed\n");
  // --- Exception handling ---
  BootLogTee::TeeFprintf("[NEVR.BOOT] installing exception handlers...\n");
  const bool vehInstalled = InstallVEH();
  InstallCrashFilterInstrumentation();
  BootLogTee::TeeFprintf("%s", VehPolicy::BootLine(vehInstalled));
  BootLogTee::TeeFprintf("[NEVR.BOOT] installing console ctrl handler...\n");
  InstallConsoleCtrlHandler();
  BootLogTee::TeeFprintf("[NEVR.PATCH] console ctrl handler installed\n");

  // NOTE: InstallResourceOverride() deferred to PreprocessCommandLineHook —
  // directory scanning deadlocks during DllMain loader lock.

  // --- Startup patches (applied before CLI parsing) ---
  BootLogTee::TeeFprintf("[NEVR.BOOT] applying startup patches...\n");
  PatchNoOvrRequiresSpectatorStream();
  PatchDeadlockMonitor();
  BootLogTee::TeeFprintf("[NEVR.PATCH] startup patches applied\n");

  // --- Wave 0 instrumentation (observation-only + EndMultiplayer crash prevention) ---
  BootLogTee::TeeFprintf("[NEVR.BOOT] initializing binary bug fix hooks...\n");
  BinaryBugFixes::Init(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress));
  BootLogTee::TeeFprintf("[NEVR.PATCH] binary bug fix hooks installed\n");

  // --- CDN asset loading ---
  // N131: moved to boot.cpp, gated `if (!g_isServer)`. g_isServer is NOT set yet
  // here (CLI is parsed later, in the PreprocessCommandLine hook), so a gate here
  // could not distinguish server from client. The call lives in boot.cpp, where g_isServer
  // is known, so a headless server never opens the CDN connection.

  // Boot phase complete — close the boot log file.  From here on, Log() and
  // the builtin_log_filter own the rotating JSONL file.  Any remaining
  // TeeFprintf calls after this write to stderr only.
  BootLogTee::TeeFprintf(
      "[NEVR.BOOT] initialization complete; continuing in %%LOCALAPPDATA%%\\EchoVR\\logs\\nevr-<timestamp>.jsonl\n");
  BootLogTee::Close();
  BuiltinLogFilter::ReplayBootTail();  // after Close nothing appends: the lines written since the main log opened (#5)

  Log(g_bootHookFailed ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
      "[NEVR.PATCH] boot hooks installed ok=%s", g_bootHookFailed ? "false" : "true");
}

static void InitializeValidatedGameModule(HMODULE module) {
  EchoVR::g_GameBaseAddress = reinterpret_cast<CHAR*>(module);
  InitializeAfterGameImageGuard();
}

void InitializeGameModule(HMODULE module) {
  GameImageGuard::RunWithSupportedGameModule(module, &InitializeValidatedGameModule);
}
