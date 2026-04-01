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

VOID Initialize() {
  if (g_initialized) return;
  g_initialized = true;

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Initializing GamePatches v%s (%s)", PROJECT_VERSION, GIT_COMMIT_HASH);
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Game Base Address: %p", EchoVR::g_GameBaseAddress);

  // Log file/product version from PE header
  CHAR filename[MAX_PATH];
  if (GetModuleFileNameA((HMODULE)EchoVR::g_GameBaseAddress, filename, MAX_PATH)) {
    DWORD handle;
    DWORD size = GetFileVersionInfoSizeA(filename, &handle);
    if (size) {
      std::vector<BYTE> buffer(size);
      if (GetFileVersionInfoA(filename, handle, size, buffer.data())) {
        VS_FIXEDFILEINFO* pFileInfo;
        UINT len;
        if (VerQueryValueA(buffer.data(), "\\", (LPVOID*)&pFileInfo, &len)) {
          Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Game File Version: %d.%d.%d.%d",
              HIWORD(pFileInfo->dwFileVersionMS), LOWORD(pFileInfo->dwFileVersionMS),
              HIWORD(pFileInfo->dwFileVersionLS), LOWORD(pFileInfo->dwFileVersionLS));
          Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Product Version: %d.%d.%d.%d",
              HIWORD(pFileInfo->dwProductVersionMS), LOWORD(pFileInfo->dwProductVersionMS),
              HIWORD(pFileInfo->dwProductVersionLS), LOWORD(pFileInfo->dwProductVersionLS));
        }
      }
    }
  }

  // Initialize the hooking library
  if (!Hooking::Initialize()) {
    MessageBoxW(NULL, L"Failed to initialize hooking library.", L"Echo Relay: Error", MB_OK);
    return;
  }

  // Early-load _local/config.json for URI redirect hooks that fire before game loads config
  LoadEarlyConfig();

  // Verify the game version before patching
  if (!VerifyGameVersion())
    MessageBoxW(NULL,
                L"NEVR version check failed. Patches may fail to be applied. Verify you're running the correct "
                L"version of Echo VR.",
                L"Echo Relay: Warning", MB_OK);

  // --- Game function hooks ---
  PatchDetour(&EchoVR::BuildCmdLineSyntaxDefinitions, reinterpret_cast<PVOID>(BuildCmdLineSyntaxDefinitionsHook));
  PatchDetour(&EchoVR::PreprocessCommandLine, reinterpret_cast<PVOID>(PreprocessCommandLineHook));
  PatchDetour(&EchoVR::NetGameSwitchState, reinterpret_cast<PVOID>(NetGameSwitchStateHook));
  PatchDetour(&EchoVR::LoadLocalConfig, reinterpret_cast<PVOID>(LoadLocalConfigHook));
  PatchDetour(&EchoVR::CJsonGetFloat, reinterpret_cast<PVOID>(CJsonGetFloatHook));
  PatchDetour(&EchoVR::HttpConnect, reinterpret_cast<PVOID>(HttpConnectHook));
  PatchDetour(&EchoVR::GetProcAddress, reinterpret_cast<PVOID>(GetProcAddressHook));
  PatchDetour(&EchoVR::SetWindowTextA_, reinterpret_cast<PVOID>(SetWindowTextAHook));
  PatchDetour(&EchoVR::JsonValueAsString, reinterpret_cast<PVOID>(JsonValueAsStringHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Service endpoint override hook installed (JsonValueAsString)");

  // --- Platform compatibility hooks ---
  InstallTLSHook();
  InstallCrashRecoveryHooks();
  InstallCreateDirectoryHooks();
  InstallWinHTTPHook();

  // --- Server crash recovery hooks ---
  InstallGameMainHook();
  InstallEntityHooks();
  InstallBugSplatHook();
  InstallGameSpaceHook();

  // --- Exception handling ---
  InstallVEH();
  InstallConsoleCtrlHandler();

  // --- Resource overrides (must install before any resource loading) ---
  InstallResourceOverride();

  // --- Load external combat patch DLL (MSVC-built, has its own MinHook) ---
  {
    CHAR dir[MAX_PATH] = {0};
    GetModuleFileNameA((HMODULE)EchoVR::g_GameBaseAddress, dir, MAX_PATH);
    CHAR* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = '\0';
    std::string path = std::string(dir) + "combatpatch.dll";
    HMODULE hCombat = LoadLibraryA(path.c_str());
    if (hCombat) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Loaded combatpatch.dll");
    } else {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] combatpatch.dll not found (optional)");
    }
  }


  // --- Startup patches (applied before CLI parsing) ---
  PatchNoOvrRequiresSpectatorStream();

  // Disable deadlock monitor unconditionally — Initialize() runs before CLI parsing
  // so g_isServer isn't set yet. Harmless on clients, prevents false panics on Wine/headless.
  PatchDeadlockMonitor();
}
