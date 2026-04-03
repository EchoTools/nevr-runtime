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

#include "common/globals.h"
#include "common/hooking.h"
#include "common/logging.h"
#include "common/echovr_functions.h"
#include "patch_addresses.h"

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

// Raw x86-64 detour without MinHook. Saves the first 14 bytes of the target,
// writes a JMP to the hook, and creates a trampoline that executes the saved
// bytes then jumps back to target+14.
//
// Trampoline layout (32 bytes, VirtualAlloc'd as executable):
//   [0..13]  original 14 bytes
//   [14]     0x48 0xB8 <target+14>  mov rax, imm64
//   [24]     0xFF 0xE0              jmp rax
//
// The caller's function pointer is updated to point to the trampoline.
static BYTE* AllocTrampoline(void* target) {
  BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!tramp) return nullptr;

  // Copy original prologue
  memcpy(tramp, target, 14);

  // JMP back to target+14
  tramp[14] = 0x48; tramp[15] = 0xB8;
  uint64_t resume = (uint64_t)target + 14;
  memcpy(&tramp[16], &resume, 8);
  tramp[24] = 0xFF; tramp[25] = 0xE0;

  return tramp;
}

template <typename T>
static void RawDetour(T* ppOriginal, void* hook) {
  void* target = (void*)*ppOriginal;

  // Create trampoline so callers can still invoke the original
  BYTE* tramp = AllocTrampoline(target);
  if (tramp) {
    *ppOriginal = (T)tramp;
  }

  // Overwrite target with JMP to hook
  BYTE jmp[14];
  jmp[0] = 0x48; jmp[1] = 0xB8; // mov rax, imm64
  memcpy(&jmp[2], &hook, 8);
  jmp[10] = 0xFF; jmp[11] = 0xE0; // jmp rax
  jmp[12] = 0x90; jmp[13] = 0x90; // nop padding
  ProcessMemcpy(target, jmp, sizeof(jmp));
}

// Phase 1: DllMain-safe initialization.
// Called from DllMain during static import. No MinHook, no LoadLibrary,
// no MessageBox, no file I/O, no Log(). Only raw memory patches.
VOID InitializeEarly() {
  fprintf(stderr, "[NEVR] Early init: v%s (%s), base=%p\n",
          PROJECT_VERSION, GIT_COMMIT_HASH, EchoVR::g_GameBaseAddress);
  fflush(stderr);

  EchoVR::InitializeFunctionPointers();

  // Raw-patch the two CLI hooks so our code runs before the game processes args.
  // RawDetour creates a trampoline and updates the function pointer so hooks
  // can still call the original.
  RawDetour(&EchoVR::BuildCmdLineSyntaxDefinitions,
            (void*)BuildCmdLineSyntaxDefinitionsHook);
  RawDetour(&EchoVR::PreprocessCommandLine,
            (void*)PreprocessCommandLineHook);

  // Raw byte patches (no hooks, just NOPs/JMPs in game code)
  PatchNoOvrRequiresSpectatorStream();
  PatchDeadlockMonitor();

  // VEH is safe during DllMain (just registers a callback)
  InstallVEH();
  InstallConsoleCtrlHandler();

  fprintf(stderr, "[NEVR] Early hooks active\n"); fflush(stderr);
}

// Phase 2: Full initialization with MinHook.
// Called from PreprocessCommandLineHook — loader lock is released, all DLLs ready.
VOID Initialize() {
  if (g_initialized) return;
  g_initialized = true;

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Initializing GamePatches v%s (%s)", PROJECT_VERSION, GIT_COMMIT_HASH);
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Game Base Address: %p", EchoVR::g_GameBaseAddress);

  if (!Hooking::Initialize()) {
    Log(EchoVR::LogLevel::Error, "[NEVR.PATCH] FATAL: Failed to initialize hooking library");
    return;
  }

  // Re-hook with proper MinHook detours (these create trampolines for calling originals)
  // BuildCmdLineSyntaxDefinitions and PreprocessCommandLine were raw-patched in phase 1.
  // The raw patches jumped directly to our hooks, but the original function pointers
  // (set by InitializeFunctionPointers) still point to the patched memory. MinHook's
  // PatchDetour creates proper trampolines. We reinstall them here.
  PatchDetour(&EchoVR::NetGameSwitchState, reinterpret_cast<PVOID>(NetGameSwitchStateHook));
  PatchDetour(&EchoVR::LoadLocalConfig, reinterpret_cast<PVOID>(LoadLocalConfigHook));
  PatchDetour(&EchoVR::CJsonGetFloat, reinterpret_cast<PVOID>(CJsonGetFloatHook));
  PatchDetour(&EchoVR::HttpConnect, reinterpret_cast<PVOID>(HttpConnectHook));
  PatchDetour(&EchoVR::GetProcAddress, reinterpret_cast<PVOID>(GetProcAddressHook));
  PatchDetour(&EchoVR::SetWindowTextA_, reinterpret_cast<PVOID>(SetWindowTextAHook));
  PatchDetour(&EchoVR::JsonValueAsString, reinterpret_cast<PVOID>(JsonValueAsStringHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Service endpoint override hook installed");

  // Platform compatibility hooks
  InstallTLSHook();
  InstallCrashRecoveryHooks();
  InstallCreateDirectoryHooks();
  InstallWinHTTPHook();

  // Server crash recovery hooks
  InstallGameMainHook();
  InstallEntityHooks();
  InstallBugSplatHook();
  InstallGameSpaceHook();

  // Resource overrides
  InstallResourceOverride();

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] All hooks installed");
}
