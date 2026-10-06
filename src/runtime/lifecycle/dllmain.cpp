// dllmain.cpp : the stable BugSplat64.dll host entry point (design
// docs/design/2026-10-05-bugsplat-nevr-split.md step 5).
//
// The game statically imports BugSplat64.dll, so its DllMain runs under the
// loader lock before WinMain. It therefore does as little as possible:
//   - validates the game image (GameImageGuard);
//   - prepares the bootstrap diagnostics and crash capture (fixed-memory only);
//   - installs ONE MinHook detour on PreprocessCommandLine (the rendezvous).
//
// It loads nevr.dll on the first rendezvous call, after attach has returned, and
// dispatches preprocess_before -> original (once) -> preprocess_after. Nothing
// else happens here: the full runtime lives in nevr.dll with a no-op DllMain.
//
// Two loading modes, preserved from the monolith:
//   1. Direct-import: the game EXE statically imports this DLL (echovr_game.dll
//      absent) — DllMain validates and installs the detour immediately.
//   2. Launcher: the game is loaded as echovr_game.dll and the external launcher
//      calls NEVR_SetGameModule(hGame) after LoadLibrary — DllMain skips the
//      detour and NEVR_SetGameModule drives the rendezvous.

#include "runtime/lifecycle/game_image_guard.h"
#include "runtime/lifecycle/system_module_loader.h"
#include "runtime/host/bootstrap_diagnostics.h"
#include "runtime/host/crash_capture.h"
#include "runtime/host/runtime_rendezvous.h"

#include <dbghelp.h>

namespace {

NevrHost::RuntimeRendezvous g_rendezvous;
NevrHost::BootstrapDiagnostics g_diagnostics;
NevrHost::CrashCapture g_capture;
HMODULE g_shim_module = nullptr;

// The always-present deferred diagnostic trigger (design F3): a host entry point
// that fires even when the rendezvous detour is absent flushes the attach-failure
// record written in fixed memory during DllMain.
void FlushDeferredAttachDiagnostic() { g_diagnostics.FlushPendingAttachDiagnostic(); }

// Validate the image and install the ONE PreprocessCommandLine detour. Any
// failure records a fixed-memory attach diagnostic (flushed later, never from
// DllMain) and leaves the game unhooked — the game statically imports this DLL,
// so a FALSE return would abort process start.
void InstallRendezvous(HMODULE game) {
  if (!GameImageGuard::IsSupportedGameModule(game)) {
    g_diagnostics.RecordAttachFailure(NevrHost::AttachFailure::kImageRejected);
    return;
  }
  g_rendezvous.Prepare(g_shim_module, game, g_diagnostics);
  void* target = reinterpret_cast<unsigned char*>(game) + NevrHost::kPreprocessRva;
  if (!NevrHost::RuntimeRendezvous::PrologueMatches(target)) {
    g_diagnostics.RecordAttachFailure(NevrHost::AttachFailure::kPrologueMismatch);
    return;
  }
  if (g_rendezvous.Install(target) == nullptr) {
    g_diagnostics.RecordAttachFailure(NevrHost::AttachFailure::kHookFailed);
    return;
  }
}

}  // namespace

// --- Legacy dbgcore.dll proxy (kept for backwards compatibility) ---

static HMODULE g_realDbgCore = nullptr;

using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
static MiniDumpWriteDump_t g_realMiniDumpWriteDump = nullptr;

static void LoadRealDbgCore() {
  if (g_realDbgCore) return;

  g_realDbgCore = Nevr::Lifecycle::LoadSystemDbgCore(GetSystemDirectoryW, LoadLibraryW);
  if (g_realDbgCore) {
    g_realMiniDumpWriteDump =
        reinterpret_cast<MiniDumpWriteDump_t>(::GetProcAddress(g_realDbgCore, "MiniDumpWriteDump"));
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
// exports.def maps the MSVC names to these C functions. createReport is the
// stable host's minimal minidump path (step 3); the constructor is the
// always-present deferred-diagnostic trigger for the no-detour case (F3).

extern "C" {
  void BugSplat_MiniDmpSender_Ctor(void*, const wchar_t*, const wchar_t*,
                                    const wchar_t*, const wchar_t*, unsigned long) {
    FlushDeferredAttachDiagnostic();
  }
  void BugSplat_MiniDmpSender_Dtor(void*) {}
  void BugSplat_createReport(void*, PEXCEPTION_POINTERS exception_pointers) {
    g_capture.Capture(exception_pointers);
  }
  unsigned long BugSplat_getFlags(void*) { return 0; }
  void BugSplat_resetAppIdentifier(void*, const wchar_t*) {}
  void BugSplat_sendAdditionalFile(void*, const wchar_t*) {}
  void BugSplat_setDefaultUserDescription(void*, const wchar_t*) {}
  void BugSplat_setDefaultUserName(void*, const wchar_t*) {}
  int BugSplat_setFlags(void*, unsigned long) { return 1; }
}

// --- Launcher entry point ---

extern "C" __declspec(dllexport) void NEVR_SetGameModule(HMODULE hGame) {
  // External launcher-mode contract: validate the game image (terminating with
  // the stable game-image-guard code on an unsupported module), then drive the
  // same rendezvous install the direct-import DllMain uses.
  GameImageGuard::RequireSupportedGameModule(hGame);
  g_rendezvous.Prepare(g_shim_module, hGame, g_diagnostics);
  void* target = reinterpret_cast<unsigned char*>(hGame) + NevrHost::kPreprocessRva;
  if (!NevrHost::RuntimeRendezvous::PrologueMatches(target)) {
    g_diagnostics.RecordAttachFailure(NevrHost::AttachFailure::kPrologueMismatch);
  } else if (g_rendezvous.Install(target) == nullptr) {
    g_diagnostics.RecordAttachFailure(NevrHost::AttachFailure::kHookFailed);
  }
  // Not under the loader lock here: flush any pending attach diagnostic now.
  FlushDeferredAttachDiagnostic();
}

// --- DLL entry point ---

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      g_shim_module = hModule;
      // Fixed-memory only under the loader lock: precompute diagnostic/crash
      // paths. No directory creation, no file I/O, no runtime load.
      g_diagnostics.Prepare();
      g_capture.Prepare(hModule);
      if (GetModuleHandleA("echovr_game.dll") == NULL) {
        // Direct-import mode: the game statically imports this DLL. Validate the
        // image and install the one detour, nothing else.
        InstallRendezvous(GetModuleHandleW(nullptr));
      }
      // Launcher mode (echovr_game.dll present): NEVR_SetGameModule drives the
      // rendezvous after LoadLibrary returns.
      break;
    case DLL_PROCESS_DETACH:
      // The runtime owns its own teardown; this host owns only the dbgcore
      // forwarder it loaded. Process-termination teardown (lpReserved != NULL) is
      // skipped by not touching thread-joining paths here at all.
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
