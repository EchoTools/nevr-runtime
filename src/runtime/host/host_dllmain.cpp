// BugSplat64.dll: the thin host (docs/adr/0008-bugsplat-host-and-nevr-runtime.md,
// docs/adr/0012-static-install-root.md).
//
// The game statically imports this DLL, so it loads at process startup before WinMain.
// DllMain only calls the host's attach (runtime/host/host_entry.h): it validates the game image and
// installs the one detour on the game's main wrapper, and it loads nothing and writes no file. The
// runtime, nevr-runtime.dll, is loaded by that detour on the game's first call into the wrapper.
//
// Supports two loading modes:
//   1. Direct import: echovr.exe imports this DLL; DllMain attaches to the process image.
//   2. Launcher mode: loaded by echovr_game.dll via LoadLibrary. DllMain only prepares;
//      NEVR_SetGameModule(hGame) is called after loading and does the rest.
#include "runtime/host/host_entry.h"
#include "runtime/lifecycle/system_module_loader.h"

#include <windows.h>

#include <dbghelp.h>

// --- Legacy dbgcore.dll proxy (kept for backwards compatibility) ---

namespace {

HMODULE g_realDbgCore = nullptr;

using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDump_t g_realMiniDumpWriteDump = nullptr;

void LoadRealDbgCore() {
  if (g_realDbgCore) return;

  g_realDbgCore = nevr::lifecycle::LoadSystemDbgCore(GetSystemDirectoryW, LoadLibraryW);
  if (g_realDbgCore) {
    g_realMiniDumpWriteDump = reinterpret_cast<MiniDumpWriteDump_t>(
        reinterpret_cast<void*>(::GetProcAddress(g_realDbgCore, "MiniDumpWriteDump")));
  }
}

}  // namespace

extern "C" BOOL WINAPI MiniDumpWriteDump(
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

// --- BugSplat MiniDmpSender methods ---
// The nine exports the game imports are in runtime/host/minidmp_sender_exports.cpp; exports.def maps them.
// The sixteen exports the runtime owns are thunks in runtime/host/runtime_exports.cpp.

// --- Launcher entry point ---

extern "C" void NEVR_SetGameModule(HMODULE hGame) {
  nevr_host::SetGameModule(hGame);
}

// --- DLL entry point ---

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      // The host's attach: validate the image and install the one detour on the game's main
      // wrapper. Nothing initializes here; the runtime loads and initializes from that detour.
      nevr_host::OnProcessAttach(hModule, GetModuleHandleW(nullptr),
                                 GetModuleHandleA("echovr_game.dll") != NULL);
      break;
    case DLL_PROCESS_DETACH:
      nevr_host::OnProcessDetach(lpReserved != NULL);  // the last chance to report a failed attach
      // Dynamic unload only (lpReserved == NULL): FreeLibrary during process termination can leave a module in
      // use after the system has run its termination code; the OS reclaims it with the process. The game
      // statically imports this DLL, so an unload of the host is not a path a running game takes.
      if (lpReserved == NULL && g_realDbgCore) {
        FreeLibrary(g_realDbgCore);
        g_realDbgCore = nullptr;
      }
      break;
  }
  return TRUE;
}

extern "C" void DetoursExportPlaceholder() {
  // An export with ordinal number 1 is needed for this library to be loaded as a suspended process.
}
