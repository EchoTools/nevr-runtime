// dllmain.cpp : Defines the entry point for the DLL application.
//
// This DLL is deployed as dbgcore.dll (DLL hijacking for early load).
// The real dbgcore.dll exports MiniDumpWriteDump, which the game's crash
// handler calls. We forward that export to the real system DLL so crash
// dumps still work.
#include "common/pch.h"
#include "asset_cdn.h"
#include "patches.h"
#include "plugin_loader.h"

#include <dbghelp.h>

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
        (MiniDumpWriteDump_t)GetProcAddress(g_realDbgCore, "MiniDumpWriteDump");
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

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      Initialize();
      break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
      if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // Only do clean shutdown on dynamic unload (lpReserved == NULL).
        // During process termination (lpReserved != NULL), threads are already
        // dead and joining would deadlock under the loader lock.
        if (lpReserved == NULL) {
          AssetCDN::Shutdown();
        }
        UnloadPlugins();
        if (g_realDbgCore) {
          FreeLibrary(g_realDbgCore);
          g_realDbgCore = nullptr;
        }
      }
      break;
  }
  return TRUE;
}

extern "C" __declspec(dllexport) void DetoursExportPlaceholder() {
  // An export with ordinal number 1 is needed for this library to be loaded as a suspended process.
}