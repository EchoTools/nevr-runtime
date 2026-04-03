// dllmain.cpp : Defines the entry point for the DLL application.
//
// Supports two loading modes:
//   1. Launcher mode: loaded by echovr.exe (our launcher) via LoadLibrary.
//      NEVR_SetGameModule(hGame) is called after loading to set the game base
//      and trigger initialization.
//   2. Legacy mode: deployed as dbgcore.dll (DLL hijacking for early load).
//      DllMain detects this and initializes immediately.
#include "common/pch.h"
#include "common/echovr_functions.h"
#include "patches.h"
#include "initialize.h"
#include "plugin_loader.h"

#include <dbghelp.h>

// --- Legacy dbgcore.dll proxy (kept for backwards compatibility) ---

static HMODULE g_realDbgCore = nullptr;

using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
static MiniDumpWriteDump_t g_realMiniDumpWriteDump = nullptr;

static void LoadRealDbgCore() {
  if (g_realDbgCore) return;

  WCHAR systemDir[MAX_PATH];
  GetSystemDirectoryW(systemDir, MAX_PATH);
  std::wstring path = std::wstring(systemDir) + L"\\dbgcore.dll";
  g_realDbgCore = LoadLibraryW(path.c_str());
  if (g_realDbgCore) {
    g_realMiniDumpWriteDump =
        (MiniDumpWriteDump_t)::GetProcAddress(g_realDbgCore, "MiniDumpWriteDump");
  }
}

extern "C" __declspec(dllexport) BOOL WINAPI MiniDumpWriteDump(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam) {
  LoadRealDbgCore();
  if (g_realMiniDumpWriteDump) {
    return g_realMiniDumpWriteDump(hProcess, ProcessId, hFile, DumpType,
                                  ExceptionParam, UserStreamParam,
                                  CallbackParam);
  }
  SetLastError(ERROR_PROC_NOT_FOUND);
  return FALSE;
}

// --- BugSplat MiniDmpSender stubs ---
// The game imports MiniDmpSender methods using MSVC name mangling.
// exports.def maps the MSVC names to these C functions.

extern "C" {
  void BugSplat_MiniDmpSender_Ctor(void*, const wchar_t*, const wchar_t*,
                                    const wchar_t*, const wchar_t*, unsigned long) {}
  void BugSplat_MiniDmpSender_Dtor(void*) {}
  void BugSplat_createReport(void*, PEXCEPTION_POINTERS) {}
  unsigned long BugSplat_getFlags(void*) { return 0; }
  void BugSplat_resetAppIdentifier(void*, const wchar_t*) {}
  void BugSplat_sendAdditionalFile(void*, const wchar_t*) {}
  void BugSplat_setDefaultUserDescription(void*, const wchar_t*) {}
  void BugSplat_setDefaultUserName(void*, const wchar_t*) {}
  int BugSplat_setFlags(void*, unsigned long) { return 1; }
}

// --- Launcher entry point ---

extern "C" __declspec(dllexport) void NEVR_SetGameModule(HMODULE hGame) {
  EchoVR::g_GameBaseAddress = (CHAR*)hGame;
  Initialize();
}

// --- DLL entry point ---

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      if (GetModuleHandleA("echovr_game.dll") == NULL) {
        EchoVR::g_GameBaseAddress = (CHAR*)GetModuleHandle(NULL);
        Initialize();
      }
      break;
    case DLL_PROCESS_DETACH:
      UnloadPlugins();
      if (g_realDbgCore) {
        FreeLibrary(g_realDbgCore);
        g_realDbgCore = nullptr;
      }
      break;
  }
  return TRUE;
}

extern "C" __declspec(dllexport) void DetoursExportPlaceholder() {
  // An export with ordinal number 1 is needed for this library to be loaded as a suspended process.
}
