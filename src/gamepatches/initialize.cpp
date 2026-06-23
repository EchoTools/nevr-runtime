#include "initialize.h"

#include <cstring>
#include <vector>

#include "asset_cdn.h"
#include "boot.h"
#include "cli.h"
#include "config.h"
#include "crash_recovery.h"
#include "gamepatches_internal.h"
#include "mode_patches.h"
// platform_compat hooks moved to module (loaded in boot.cpp)
#include "resource_override.h"
#include "state_machine.h"
#include "ws_bridge.h"

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

#include <windows.h>

// ============================================================================
// Internal state
// ============================================================================

static BOOL g_initialized = FALSE;

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
// + ServerLib vtable shim for MinGW/MSVC ABI compatibility
// ============================================================================

// The game calls IServerLib methods via vtable dispatch: call [rax+N].
// MinGW and MSVC generate incompatible vtable layouts (RTTI, typeinfo
// offsets differ). Instead of letting the game use our C++ vtable, we
// intercept GetProcAddress("ServerLib") and return a shim factory that
// produces a raw C-layout struct with function pointers at the exact
// byte offsets the game expects. No RTTI, no typeinfo, no compiler magic.
//
// The game sees: obj -> vtable_ptr -> [fn0, fn1, fn2, ...]
// We provide:    shim -> raw_array -> [thunk0, thunk1, thunk2, ...]

// ============================================================================
// pnsdemo.dll user ID override
// ============================================================================
// CNSIUser vtable layout (from RE agent session spritzs-echovr-re, 2026-06-18):
//   Slot  0 (+0x00): GetLoggedInUser — returns per-login CNSDemoUser* (0xC0 bytes)
//   Slot 13 (+0x68): GetLoggedInUserID — reads uint64 at sub-object+0x88
//   The sub-object is allocated in slot 0, cached globally.
//   pnsdemo.dll slot 0 impl: 0x18007ee60
//   pnsdemo.dll slot 13 impl: 0x180082d40
//   echovr.exe consumption site: 0x140606ea8 (XPID construction "%s-%llu")
//
// TODO(revault): import these findings into ReVault when it's rebuilt:
//   - CNSDemoUser sub-object: 0xC0 bytes, vtable at +0x00, user_id at +0x88
//   - CNSDemoUsers manager: 0x428 bytes, vtable at +0x00
//   - CNSIUser vtable: 32 slots, slot 0 = GetLoggedInUser, slot 13 = GetLoggedInUserID

typedef void* (*PnsdemoSlot0Fn)(void* self);
static PnsdemoSlot0Fn g_pnsdemo_orig_slot0 = nullptr;
static uint64_t g_pnsdemo_discord_id = 0;

static void* PnsdemoGetLoggedInUserHook(void* self) {
  void* user = g_pnsdemo_orig_slot0(self);
  if (user && g_pnsdemo_discord_id != 0) {
    uint64_t* id_field = reinterpret_cast<uint64_t*>(
        reinterpret_cast<uint8_t*>(user) + 0x88);
    uint64_t old_id = *id_field;
    *id_field = g_pnsdemo_discord_id;
    static bool logged = false;
    if (!logged) {
      fprintf(stderr, "[NEVR.DLLHOOK] pnsdemo: user ID overridden %llu → %llu\n",
              (unsigned long long)old_id, (unsigned long long)g_pnsdemo_discord_id);
      fflush(stderr);
      logged = true;
    }
  }
  return user;
}

// ============================================================================
// ServerLib vtable shim
// ============================================================================

struct ServerLibShim {
  void** vtable;
  // 0x50 bytes of padding to match original 0x58 total size
  uint8_t padding[0x50];
};

static void* g_shim_vtable[14] = {};
static ServerLibShim g_shim = {};
static void* g_real_serverlib = nullptr;

// Thunks: call the real GameServerLib methods via GetProcAddress on our DLL.
// Each thunk strips the shim 'this' and forwards to the real object.
// x64 calling convention: rcx=this, rdx=arg1, r8=arg2, r9=arg3, stack=rest

typedef INT64  (*Fn_UnkFunc0)(void*, void*, INT64, INT64);
typedef void*  (*Fn_Initialize)(void*, void*, void*, void*, const char*);
typedef void   (*Fn_Terminate)(void*);
typedef void   (*Fn_Update)(void*);
typedef void   (*Fn_UnkFunc1)(void*, UINT64);
typedef void   (*Fn_RequestReg)(void*, INT64, char*, UINT64, UINT64, const void*);
typedef void   (*Fn_Unregister)(void*);
typedef void   (*Fn_EndSession)(void*);
typedef void   (*Fn_LockPlayers)(void*);
typedef void   (*Fn_UnlockPlayers)(void*);
typedef void   (*Fn_AcceptPlayers)(void*, void*);
typedef void   (*Fn_RemovePlayer)(void*, void*);

static void** g_real_vtable = nullptr;

#define REAL_CALL(slot, type, ...) \
  (reinterpret_cast<type>(g_real_vtable[slot])(g_real_serverlib, ##__VA_ARGS__))

static INT64 Shim_UnkFunc0(void*, void* a, INT64 b, INT64 c) { return REAL_CALL(0, Fn_UnkFunc0, a, b, c); }
static void* Shim_Initialize(void*, void* a, void* b, void* c, const char* d) { return REAL_CALL(1, Fn_Initialize, a, b, c, d); }
static void  Shim_Terminate(void*) { REAL_CALL(2, Fn_Terminate); }
static void  Shim_Update(void*) { REAL_CALL(3, Fn_Update); }
static void  Shim_UnkFunc1(void*, UINT64 a) { REAL_CALL(4, Fn_UnkFunc1, a); }
static void  Shim_RequestReg(void*, INT64 a, char* b, UINT64 c, UINT64 d, const void* e) { REAL_CALL(5, Fn_RequestReg, a, b, c, d, e); }
static void  Shim_Unregister(void*) { REAL_CALL(6, Fn_Unregister); }
static void  Shim_EndSession(void*) { REAL_CALL(7, Fn_EndSession); }
static void  Shim_LockPlayers(void*) { REAL_CALL(8, Fn_LockPlayers); }
static void  Shim_UnlockPlayers(void*) { REAL_CALL(9, Fn_UnlockPlayers); }
static void  Shim_AcceptPlayers(void*, void* a) { REAL_CALL(10, Fn_AcceptPlayers, a); }
static void  Shim_RemovePlayer(void*, void* a) { REAL_CALL(11, Fn_RemovePlayer, a); }

static void* ShimServerLib() {
  if (!g_real_serverlib) {
    // Get the real ServerLib from the gameserver DLL
    HMODULE dll = GetModuleHandleA("pnsradgameserver.dll");
    if (!dll) dll = GetModuleHandleA("pnsradgameserver");
    if (!dll) {
      fprintf(stderr, "[NEVR.SHIM] pnsradgameserver.dll not loaded\n"); fflush(stderr);
      return nullptr;
    }

    typedef void* (*RealServerLibFn)();
    RealServerLibFn real_fn = reinterpret_cast<RealServerLibFn>(
        EchoVR::GetProcAddress(dll, "ServerLib"));
    if (!real_fn) {
      fprintf(stderr, "[NEVR.SHIM] ServerLib export not found\n"); fflush(stderr);
      return nullptr;
    }

    g_real_serverlib = real_fn();
    g_real_vtable = *reinterpret_cast<void***>(g_real_serverlib);

    fprintf(stderr, "[NEVR.SHIM] real obj=%p vtable=%p\n",
            g_real_serverlib, (void*)g_real_vtable); fflush(stderr);

    // Build shim vtable — raw function pointers at known offsets
    g_shim_vtable[0]  = reinterpret_cast<void*>(&Shim_UnkFunc0);
    g_shim_vtable[1]  = reinterpret_cast<void*>(&Shim_Initialize);
    g_shim_vtable[2]  = reinterpret_cast<void*>(&Shim_Terminate);
    g_shim_vtable[3]  = reinterpret_cast<void*>(&Shim_Update);
    g_shim_vtable[4]  = reinterpret_cast<void*>(&Shim_UnkFunc1);
    g_shim_vtable[5]  = reinterpret_cast<void*>(&Shim_RequestReg);
    g_shim_vtable[6]  = reinterpret_cast<void*>(&Shim_Unregister);
    g_shim_vtable[7]  = reinterpret_cast<void*>(&Shim_EndSession);
    g_shim_vtable[8]  = reinterpret_cast<void*>(&Shim_LockPlayers);
    g_shim_vtable[9]  = reinterpret_cast<void*>(&Shim_UnlockPlayers);
    g_shim_vtable[10] = reinterpret_cast<void*>(&Shim_AcceptPlayers);
    g_shim_vtable[11] = reinterpret_cast<void*>(&Shim_RemovePlayer);

    g_shim.vtable = g_shim_vtable;
    memset(g_shim.padding, 0, sizeof(g_shim.padding));

    fprintf(stderr, "[NEVR.SHIM] shim=%p shim_vtable=%p Initialize=%p\n",
            (void*)&g_shim, (void*)g_shim_vtable,
            g_shim_vtable[1]); fflush(stderr);
  }
  return &g_shim;
}

static void* LazyPnsradUsers();  // forward decl — defined below CSysDLL_GetSymbol section

static FARPROC GetProcAddressHook(HMODULE hModule, LPCSTR lpProcName) {
  // Platform DLLs (pnsdemo/pnsovr) crash during RadPluginShutdown due to freed memory.
  // Detect platform DLLs by checking for the "Users" export they all define.
  if (g_isServer && strcmp(lpProcName, "RadPluginShutdown") == 0) {
    if (EchoVR::GetProcAddress(hModule, "Users") != NULL) exit(0);
  }
  return EchoVR::GetProcAddress(hModule, lpProcName);
}

// ============================================================================
// CSysDLL_GetSymbol hook — intercepts game's internal DLL symbol resolution
// to provide MinGW/MSVC vtable-compatible shim for ServerLib
// ============================================================================
// CSysDLL_GetSymbol @ 0x1400eaef0 — the game's own GetProcAddress wrapper.
// Used by CNSLobby_LoadServerSupport to resolve "ServerLib" from pnsradgameserver.dll.

// Lazy Users proxy: returns pnsrad's Users object when called, falling back to
// pnsdemo's if pnsrad isn't loaded yet. This handles the ordering problem where
// pnsdemo loads and the game queries Users before pnsrad exists.
static void* LazyPnsradUsers() {
  HMODULE pnsrad = GetModuleHandleA("pnsrad.dll");
  if (pnsrad) {
    typedef void* (*UsersFn)();
    UsersFn fn = reinterpret_cast<UsersFn>(GetProcAddress(pnsrad, "Users"));
    if (fn) {
      void* obj = fn();
      static bool logged = false;
      if (!logged) {
        fprintf(stderr, "[NEVR.SHIM] LazyPnsradUsers → pnsrad Users obj=%p\n", obj);
        fflush(stderr);
        logged = true;
      }
      return obj;
    }
  }
  // pnsrad not loaded yet — shouldn't happen if called after init
  fprintf(stderr, "[NEVR.SHIM] LazyPnsradUsers — pnsrad.dll not loaded, returning null\n");
  fflush(stderr);
  return nullptr;
}

typedef void* (*CSysDLL_GetSymbol_fn)(void* dll_handle, const char* symbol_name);
static CSysDLL_GetSymbol_fn g_original_GetSymbol = nullptr;

static void* CSysDLL_GetSymbolHook(void* dll_handle, const char* symbol_name) {
  void* result = g_original_GetSymbol(dll_handle, symbol_name);

  if (symbol_name && strcmp(symbol_name, "ServerLib") == 0 && result != nullptr) {
    static bool logged = false;
    if (!logged) {
      fprintf(stderr, "[NEVR.SHIM] CSysDLL_GetSymbol('ServerLib') → passthrough (no vtable shim)\n");
      fflush(stderr);
      logged = true;
    }
    // Return the real ServerLib directly — MinGW/MSVC single-inheritance
    // vtable layouts are compatible for this case.
    return result;
  }

  // XInput stubs — return "not connected" for headless server.
  // The game resolves XInputGetState/XInputGetCapabilities via CSysDLL_GetSymbol.
  if (symbol_name && result != nullptr) {
    static auto xinput_get_state = +[](uint32_t, void*) -> uint32_t { return 0x48F; /* ERROR_DEVICE_NOT_CONNECTED */ };
    static auto xinput_set_state = +[](uint32_t, void*) -> uint32_t { return 0x48F; };
    static auto xinput_get_caps  = +[](uint32_t, uint32_t, void*) -> uint32_t { return 0x48F; };

    if (strcmp(symbol_name, "XInputGetState") == 0) {
      static bool logged = false;
      if (!logged) { fprintf(stderr, "[NEVR.SHIM] XInputGetState → stub (not connected)\n"); fflush(stderr); logged = true; }
      return reinterpret_cast<void*>(xinput_get_state);
    }
    if (strcmp(symbol_name, "XInputSetState") == 0) return reinterpret_cast<void*>(xinput_set_state);
    if (strcmp(symbol_name, "XInputGetCapabilities") == 0) return reinterpret_cast<void*>(xinput_get_caps);
  }

  // Proxy pnsdemo's "Users" export → lazy wrapper that returns pnsrad's Users.
  // At resolution time pnsrad.dll may not be loaded yet, so we return a wrapper
  // function that resolves pnsrad's Users on first call.
  if (symbol_name && strcmp(symbol_name, "Users") == 0 && result != nullptr) {
    static void* pnsdemo_users_result = result; // save pnsdemo's original
    static bool logged = false;
    if (!logged) {
      fprintf(stderr, "[NEVR.SHIM] intercepting 'Users' export → lazy pnsrad proxy\n");
      fflush(stderr);
      logged = true;
    }
    return reinterpret_cast<void*>(&LazyPnsradUsers);
  }

  return result;
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

  fprintf(stderr, "[NEVR.PATCH] Initializing v%s base=%p\n", PROJECT_VERSION, EchoVR::g_GameBaseAddress);
  fflush(stderr);

  if (!VerifyGameVersion()) {
    fprintf(stderr, "[NEVR] WARNING: game binary version mismatch — hooks may crash\n"); fflush(stderr);
  }

  EchoVR::InitializeFunctionPointers();
  fprintf(stderr, "[NEVR] fn ptrs OK\n"); fflush(stderr);

  if (!Hooking::Initialize()) {
    fprintf(stderr, "[NEVR] FATAL: hooking init failed\n");
    return;
  }
  fprintf(stderr, "[NEVR] minhook OK, hooking...\n"); fflush(stderr);

  // --- DLL load interceptor (patch DLLs as they load) ---
  DllLoadHook::Install();
  fprintf(stderr, "[NEVR] DLL load hooks OK\n"); fflush(stderr);

  // --- Headless graphics stubs (DXGI/D3D11 interception) ---
  // Register callbacks now so they fire when the game loads dxgi.dll/d3d11.dll.
  // g_isHeadless may not be set yet (CLI not parsed), but the hook checks it at
  // call time — if the game is running in headless mode the stubs activate,
  // otherwise they pass through to real DirectX.
  InstallHeadlessGraphicsHooks();
  fprintf(stderr, "[NEVR] headless graphics hooks registered\n"); fflush(stderr);

  // Register pnsdemo.dll patcher: override user ID with Discord ID from config.
  //
  // The game calls CNSIUser vtable slot 13 (GetLoggedInUserID, +0x68) to get the
  // numeric part of the XPID. The user ID lives at +0x88 in a per-login sub-object
  // (0xC0 bytes) created by manager vtable slot 0 (GetLoggedInUser).
  //
  // Hook: intercept manager slot 0, call original, write Discord ID to ret+0x88.
  // Confirmed by RE agent: echovr.exe reads slot 13 at 0x140606ea8, pnsdemo slot 0
  // at 0x18007ee60, field written at sub-object+0x88, read by slot 13 at 0x180082d40.
  DllLoadHook::OnLoad("pnsdemo.dll", [](const char*, HMODULE module) {
    CHAR* cfgDiscordId = nullptr;
    if (g_earlyConfigPtr) {
      cfgDiscordId = EchoVR::JsonValueAsString(g_earlyConfigPtr, (CHAR*)"nevr_discord_id", NULL, false);
    }
    if (!cfgDiscordId || cfgDiscordId[0] == '\0') {
      fprintf(stderr, "[NEVR.DLLHOOK] pnsdemo: no nevr_discord_id in config, skipping\n");
      fflush(stderr);
      return;
    }

    g_pnsdemo_discord_id = strtoull(cfgDiscordId, nullptr, 10);

    typedef void* (*UsersFn)();
    UsersFn getUsersObj = reinterpret_cast<UsersFn>(GetProcAddress(module, "Users"));
    if (!getUsersObj) {
      fprintf(stderr, "[NEVR.DLLHOOK] pnsdemo: no 'Users' export\n"); fflush(stderr);
      return;
    }

    void* usersObj = getUsersObj();
    if (!usersObj) {
      fprintf(stderr, "[NEVR.DLLHOOK] pnsdemo: Users() returned null\n"); fflush(stderr);
      return;
    }

    // Read the vtable from the Users manager object
    void** vtable = *reinterpret_cast<void***>(usersObj);
    if (!vtable) {
      fprintf(stderr, "[NEVR.DLLHOOK] pnsdemo: Users vtable is null\n"); fflush(stderr);
      return;
    }

    // Save original slot 0 (GetLoggedInUser) and patch the vtable
    g_pnsdemo_orig_slot0 = reinterpret_cast<PnsdemoSlot0Fn>(vtable[0]);

    DWORD oldProtect;
    if (VirtualProtect(&vtable[0], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
      vtable[0] = reinterpret_cast<void*>(&PnsdemoGetLoggedInUserHook);
      VirtualProtect(&vtable[0], sizeof(void*), oldProtect, &oldProtect);
      fprintf(stderr, "[NEVR.DLLHOOK] pnsdemo: patched vtable slot 0 — user ID will be %llu\n",
              (unsigned long long)g_pnsdemo_discord_id);
    } else {
      fprintf(stderr, "[NEVR.DLLHOOK] pnsdemo: VirtualProtect failed on vtable\n");
    }
    fflush(stderr);
  });

  // CSysDLL_GetSymbol hook REMOVED — game calls ServerLib directly through the
  // normal DLL path. The shim/proxy approach caused vtable corruption.
  // pnsrad_enabler handles pnsovr→pnsrad via binary string patching instead.
  fprintf(stderr, "[NEVR] CSysDLL_GetSymbol hook SKIPPED (direct ServerLib path)\n"); fflush(stderr);

  // --- Broadcaster dispatch guard ---
  BroadcasterGuard::Install(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress));
  fprintf(stderr, "[NEVR] broadcaster guard OK\n"); fflush(stderr);

  // --- Log filter (hooks CLog::PrintfImpl to capture/filter/file game output) ---
  // g_isServer not set yet (CLI not parsed); pass false — log filter works regardless
  BuiltinLogFilter::Init(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress), false);
  fprintf(stderr, "[NEVR] log filter OK\n"); fflush(stderr);

  // --- Game function hooks ---
  fprintf(stderr, "[NEVR] BuildCmdLine target=%p\n", (void*)EchoVR::BuildCmdLineSyntaxDefinitions); fflush(stderr);
  BOOL r1 = Hooking::Attach(reinterpret_cast<PVOID*>(&EchoVR::BuildCmdLineSyntaxDefinitions),
                             reinterpret_cast<PVOID>(BuildCmdLineSyntaxDefinitionsHook));
  fprintf(stderr, "[NEVR] BuildCmdLine hook: %s\n", r1 ? "OK" : "FAILED"); fflush(stderr);
  BOOL r2 = Hooking::Attach(reinterpret_cast<PVOID*>(&EchoVR::PreprocessCommandLine),
                             reinterpret_cast<PVOID>(PreprocessCommandLineHook));
  fprintf(stderr, "[NEVR] PreprocessCmd hook: %s\n", r2 ? "OK" : "FAILED"); fflush(stderr);
  PatchDetour(&EchoVR::NetGameSwitchState, reinterpret_cast<PVOID>(NetGameSwitchStateHook));
  PatchDetour(&EchoVR::LoadLocalConfig, reinterpret_cast<PVOID>(LoadLocalConfigHook));
  PatchDetour(&EchoVR::CJsonGetFloat, reinterpret_cast<PVOID>(CJsonGetFloatHook));
  PatchDetour(&EchoVR::HttpConnect, reinterpret_cast<PVOID>(HttpConnectHook));
  PatchDetour(&EchoVR::GetProcAddress, reinterpret_cast<PVOID>(GetProcAddressHook));
  PatchDetour(&EchoVR::SetWindowTextA_, reinterpret_cast<PVOID>(SetWindowTextAHook));
  PatchDetour(&EchoVR::JsonValueAsString, reinterpret_cast<PVOID>(JsonValueAsStringHook));
  fprintf(stderr, "[NEVR] game hooks OK\n"); fflush(stderr);
  // --- Platform compatibility hooks ---
  // InstallTLSHook() not needed — WebSocket bridge handles TLS via ixwebsocket.
  // WinHTTP hook (InstallWinHTTPHook) handles TLS for HTTP/REST calls via curl.
  // WebSocket bridge (InstallWebSocketBridge) is started in PreprocessCommandLineHook
  // after config is loaded — it needs the wss:// URI from config.json.
  fprintf(stderr, "[NEVR] tls: ws bridge deferred to boot\n"); fflush(stderr);
  InstallCrashRecoveryHooks();
  fprintf(stderr, "[NEVR] crash OK\n"); fflush(stderr);
  // CreateDirectory + WinHTTP hooks moved to platform_compat module (loaded in boot.cpp)
  fprintf(stderr, "[NEVR] platform hooks deferred to module\n"); fflush(stderr);

  // --- Server crash recovery hooks ---
  InstallGameMainHook();
  InstallEntityHooks();
  InstallBugSplatHook();
  InstallGameSpaceHook();
  fprintf(stderr, "[NEVR] server hooks OK\n"); fflush(stderr);
  // --- Exception handling ---
  InstallVEH();
  fprintf(stderr, "[NEVR] veh OK\n"); fflush(stderr);
  InstallConsoleCtrlHandler();
  fprintf(stderr, "[NEVR] console OK\n"); fflush(stderr);

  // NOTE: InstallResourceOverride() deferred to PreprocessCommandLineHook —
  // directory scanning deadlocks during DllMain loader lock.

  // --- Startup patches (applied before CLI parsing) ---
  PatchNoOvrRequiresSpectatorStream();
  PatchDeadlockMonitor();
  fprintf(stderr, "[NEVR] patches OK\n"); fflush(stderr);

  // --- Wave 0 instrumentation (observation-only + EndMultiplayer crash prevention) ---
  Wave0::Init(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress));
  fprintf(stderr, "[NEVR] wave0 OK\n"); fflush(stderr);

  // --- CDN asset loading ---
  AssetCDN::Initialize();

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] All hooks installed");
}
