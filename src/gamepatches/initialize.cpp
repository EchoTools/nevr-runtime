#include "initialize.h"

#include <cstring>
#include <vector>

#include "asset_cdn.h"
#include "boot_log_tee.h"
#include "boot.h"
#include "cli.h"
#include "config.h"
#include "crash_recovery.h"
#include "gamepatches_internal.h"
#include "mode_patches.h"
// platform_compat lives in src/modules/platform-compat (loaded in boot.cpp).
// The gamepatches copy was deleted 2026-07-26: never compiled, zero call sites.
#include "resource_override.h"
#include "state_machine.h"
#include "ws_bridge.h"
#include "gameserver/gameserver.h"

#include "broadcaster_guard.h"
#include "builtin_log_filter.h"
#include "dll_load_hook.h"
#include "headless_graphics.h"
#include "common/globals.h"
#include "common/hooking.h"
#include "common/logging.h"
#include "common/echovr_functions.h"
#include "patch_addresses.h"
#include "wave0_instrumentation.h"
#include "xpid_patch.h"

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
// GetProcAddress hook — prevents server crash during platform DLL shutdown
// ============================================================================

static FARPROC GetProcAddressHook(HMODULE hModule, LPCSTR lpProcName) {
  // Platform DLLs (pnsdemo/pnsovr) crash during RadPluginShutdown due to freed memory.
  // Detect platform DLLs by checking for the "Users" export they all define.
  if (g_isServer && strcmp(lpProcName, "RadPluginShutdown") == 0) {
    if (EchoVR::GetProcAddress(hModule, "Users") != NULL) exit(0);
  }
  return EchoVR::GetProcAddress(hModule, lpProcName);
}

// ============================================================================
// GameServerLib factory — provides IServerLib to the game via CSysDLL_GetSymbol
// ============================================================================

static EchoVR::IServerLib* g_ServerLib = nullptr;

static EchoVR::IServerLib* ServerLibFactory() {
    if (!g_ServerLib) {
        g_ServerLib = new GameServerLib();
        BootLogTee::TeeFprintf("[NEVR.GAMESERVER] ServerLib() created obj=%p\n", (void*)g_ServerLib);
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

static void* CSysDLL_GetSymbolHook(void* dll_handle, const char* symbol_name) {
  if (symbol_name && strcmp(symbol_name, "ServerLib") == 0) {
    static bool logged = false;
    if (!logged) {
        BootLogTee::TeeFprintf("[NEVR.GAMESERVER] serverlib symbol resolved -> gamepatches factory\n");
        logged = true;
    }
    return reinterpret_cast<void*>(&ServerLibFactory);
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

  return result;
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

static BOOL VerifyGameVersion() {
#define IMG_SIGNATURE_OFFSET 0x3C
#define IMG_SIGNATURE_SIZE 0x04

  DWORD* signatureOffset = (DWORD*)(EchoVR::g_GameBaseAddress + IMG_SIGNATURE_OFFSET);
  IMAGE_FILE_HEADER* coffFileHeader =
      (IMAGE_FILE_HEADER*)(EchoVR::g_GameBaseAddress + (*signatureOffset + IMG_SIGNATURE_SIZE));

  // Echo VR version 34.4.631547.1 — Wednesday, May 3, 2023 10:28:06 PM
  return coffFileHeader->TimeDateStamp == 0x6452dff6;
}

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
// Main initialization
// ============================================================================

VOID Initialize() {
  if (g_initialized) return;
  g_initialized = true;

  // Open the boot log BEFORE the first fprintf — all boot messages tee to both
  // stderr and nevr-boot.jsonl so early failures leave a record.  Uses only
  // kernel32 (CreateFileA / WriteFile), safe under the loader lock on a
  // static-CRT build (see boot_log_tee.h DESIGN DECISION and N43).
  BootLogTee::Init();

  BootLogTee::TeeFprintf("[NEVR.PATCH] Initializing v%s base=%p\n", PROJECT_VERSION, EchoVR::g_GameBaseAddress);
  if (!VerifyGameVersion()) {
    BootLogTee::TeeFprintf("[NEVR.PATCH] game binary version mismatch — hooks may crash\n");
  }

  EchoVR::InitializeFunctionPointers();
  BootLogTee::TeeFprintf("[NEVR.PATCH] function pointers resolved\n");

  if (!Hooking::Initialize()) {
    BootLogTee::TeeFprintf("[NEVR.PATCH] FATAL hooking init failed\n");
    g_bootHookFailed = true;
    return;
  }
  BootLogTee::TeeFprintf("[NEVR.PATCH] minhook initialized\n");

  // --- DLL load interceptor (patch DLLs as they load) ---
  DllLoadHook::Install();
  BootLogTee::TeeFprintf("[NEVR.PATCH] dll load hooks installed\n");

  // --- Headless graphics stubs (DXGI/D3D11 interception) ---
  // Register callbacks now so they fire when the game loads dxgi.dll/d3d11.dll.
  // g_isHeadless may not be set yet (CLI not parsed), but the hook checks it at
  // call time — if the game is running in headless mode the stubs activate,
  // otherwise they pass through to real DirectX.
  InstallHeadlessGraphicsHooks();
  BootLogTee::TeeFprintf("[NEVR.HEADLESS] graphics hooks registered\n");

  // N59: re-wire PatchDscProvider — the call site was lost when N43's
  // Initialize() rewrite merged over N41's include+call (a6bb57d).
  // Without this, the 5-site PSN→DSC + ???→DSC string-table rewrite
  // never executes — game sends PSN-/???- instead of DSC- in provider
  // strings (RULINGS.md 2026-07-20 login-prefix).
  PatchDscProvider();

  {
      void* sym_target = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress) + (0x1400eaef0 - 0x140000000));
      if (MH_CreateHook(sym_target, reinterpret_cast<void*>(&CSysDLL_GetSymbolHook),
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
  BroadcasterGuard::Install(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress));
  // Truthful outcome: Install() is an empty placeholder (broadcaster_guard.cpp).
  // The previous line here read "broadcaster guard installed" — a log line
  // asserting a fact that is false in the source it describes. An operator (or
  // an agent) reading it would conclude a dispatch guard exists. None does.
  BootLogTee::TeeFprintf("[NEVR.PATCH] broadcaster guard: no-op placeholder, nothing installed\n");

  // --- Log filter (hooks CLog::PrintfImpl to capture/filter/file game output) ---
  // g_isServer not set yet (CLI not parsed); pass false — log filter works regardless
  BuiltinLogFilter::Init(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress), false);
  BootLogTee::TeeFprintf("[NEVR.PATCH] log filter installed\n");

  // --- Game function hooks ---
  BootLogTee::TeeFprintf("[NEVR.PATCH] hooking name=BuildCmdLineSyntaxDefinitions target=%p\n", (void*)EchoVR::BuildCmdLineSyntaxDefinitions);
  BOOL r1 = Hooking::Attach(reinterpret_cast<PVOID*>(&EchoVR::BuildCmdLineSyntaxDefinitions),
                             reinterpret_cast<PVOID>(BuildCmdLineSyntaxDefinitionsHook));
  BootLogTee::TeeFprintf("[NEVR.PATCH] hook name=BuildCmdLineSyntaxDefinitions result=%s\n", r1 ? "OK" : "FAILED");
  if (!r1) g_bootHookFailed = true;
  BOOL r2 = Hooking::Attach(reinterpret_cast<PVOID*>(&EchoVR::PreprocessCommandLine),
                             reinterpret_cast<PVOID>(PreprocessCommandLineHook));
  BootLogTee::TeeFprintf("[NEVR.PATCH] hook name=PreprocessCommandLine result=%s\n", r2 ? "OK" : "FAILED");
  if (!r2) g_bootHookFailed = true;
  PatchDetour(&EchoVR::NetGameSwitchState, reinterpret_cast<PVOID>(NetGameSwitchStateHook));
  PatchDetour(&EchoVR::LoadLocalConfig, reinterpret_cast<PVOID>(LoadLocalConfigHook));
  PatchDetour(&EchoVR::CJsonGetFloat, reinterpret_cast<PVOID>(CJsonGetFloatHook));
  PatchDetour(&EchoVR::HttpConnect, reinterpret_cast<PVOID>(HttpConnectHook));
  PatchDetour(&EchoVR::GetProcAddress, reinterpret_cast<PVOID>(GetProcAddressHook));
  PatchDetour(&EchoVR::SetWindowTextA_, reinterpret_cast<PVOID>(SetWindowTextAHook));
  PatchDetour(&EchoVR::JsonValueAsString, reinterpret_cast<PVOID>(JsonValueAsStringHook));
  BootLogTee::TeeFprintf("[NEVR.PATCH] game hooks installed\n");
  // --- Platform compatibility hooks ---
  // InstallTLSHook() not needed — WebSocket bridge handles TLS via ixwebsocket.
  // WinHTTP hook (InstallWinHTTPHook) handles TLS for HTTP/REST calls via curl.
  // WebSocket bridge (InstallWebSocketBridge) is started in PreprocessCommandLineHook
  // after config is loaded — it needs the wss:// URI from config.json.
  BootLogTee::TeeFprintf("[NEVR.PATCH] tls deferred=ws_bridge stage=boot\n");
  InstallCrashRecoveryHooks();
  BootLogTee::TeeFprintf("[NEVR.CRASH] crash recovery hooks installed\n");
  // CreateDirectory + WinHTTP hooks moved to platform_compat module (loaded in boot.cpp)
  BootLogTee::TeeFprintf("[NEVR.PATCH] platform hooks deferred=platform_compat_module\n");

  // --- Server crash recovery hooks ---
  InstallGameMainHook();
  InstallEntityHooks();
  InstallBugSplatHook();
  InstallGameSpaceHook();
  BootLogTee::TeeFprintf("[NEVR.PATCH] server crash-recovery hooks installed\n");
  // --- Exception handling ---
  InstallVEH();
  InstallCrashFilterInstrumentation();
  BootLogTee::TeeFprintf("[NEVR.CRASH] veh installed\n");
  InstallConsoleCtrlHandler();
  BootLogTee::TeeFprintf("[NEVR.PATCH] console ctrl handler installed\n");

  // NOTE: InstallResourceOverride() deferred to PreprocessCommandLineHook —
  // directory scanning deadlocks during DllMain loader lock.

  // --- Startup patches (applied before CLI parsing) ---
  PatchNoOvrRequiresSpectatorStream();
  PatchDeadlockMonitor();
  BootLogTee::TeeFprintf("[NEVR.PATCH] startup patches applied\n");

  // --- Wave 0 instrumentation (observation-only + EndMultiplayer crash prevention) ---
  Wave0::Init(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress));
  BootLogTee::TeeFprintf("[NEVR.PATCH] wave0 instrumentation installed\n");

  // --- CDN asset loading ---
  AssetCDN::Initialize();

  // Boot phase complete — close the boot log file.  From here on, Log() and
  // the builtin_log_filter own the rotating JSONL file.  Any remaining
  // TeeFprintf calls after this write to stderr only.
  BootLogTee::Close();

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] All hooks installed");
}
