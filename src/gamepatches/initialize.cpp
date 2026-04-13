#include "initialize.h"

#include <cstring>
#include <vector>

#include "boot.h"
#include "cli.h"
#include "config.h"
#include "crash_recovery.h"
#include "gamepatches_internal.h"
#include "mode_patches.h"
#include "platform_compat.h"
#include "resource_override.h"
#include "state_machine.h"
#include "ws_bridge.h"

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

  EchoVR::InitializeFunctionPointers();
  fprintf(stderr, "[NEVR] fn ptrs OK\n"); fflush(stderr);

  if (!Hooking::Initialize()) {
    fprintf(stderr, "[NEVR] FATAL: hooking init failed\n");
    return;
  }
  fprintf(stderr, "[NEVR] minhook OK, hooking...\n"); fflush(stderr);

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
  InstallCreateDirectoryHooks();
  InstallWinHTTPHook();
  fprintf(stderr, "[NEVR] platform OK\n"); fflush(stderr);

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

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] All hooks installed");
}
