// BugSplat64.dll shim for the attach-time proof gate: the PRODUCTION host units
// (runtime/host/host_entry.h) behind a DllMain, so the gate measures the attach path the
// game runs (docs/adr/0008-bugsplat-host-and-nevr-runtime.md, "Proof gate").
//
// DLL_PROCESS_ATTACH calls nevr_host::OnProcessAttach, which validates the game image,
// validates the GameMainWrapper prologue, installs ONE MinHook detour and journals each
// decision. It loads nothing and does no file I/O. Any validation or MinHook failure
// leaves the game unhooked and still returns TRUE: the game statically imports this DLL,
// so a FALSE return would abort process start.
//
// Test seams (resolved from the executable; absent in production):
//   - the game image comes from BootstrapProbeGameImage instead of
//     GetModuleHandleW(nullptr), because the harness cannot be echovr.exe;
//   - BootstrapProbeAttachHoldMs keeps DllMain open after the hook is live, to force a
//     concurrent detour entry while the loader lock is held;
//   - nevr_host_test_FlushDeferred stands for the crash-reporter constructor, the trigger
//     that reports the attach decisions when no detour exists to report them.

#include "runtime/tests/bootstrap_attach/abi.h"

#include "runtime/host/host_entry.h"

namespace {

using namespace nevr_bootstrap_probe;

RecordEventFn g_record = nullptr;

void Record(Event event, std::uint64_t payload = 0) {
  if (g_record != nullptr) g_record(event, payload);
}

}  // namespace

// Ordinal 1, as in the production exports.def. The static-import harness binds to it.
extern "C" __declspec(dllexport) void DetoursExportPlaceholder() {}

extern "C" __declspec(dllexport) void nevr_host_test_FlushDeferred() {
  nevr_host::OnCrashReporterConstructed();
}

// The attach failure path flushes the journal from a thread (host_entry.cpp); the test reads the file only after
// that thread is done, so it never races a half-written flush.
extern "C" __declspec(dllexport) int nevr_host_test_WaitFailureFlush() {
  return nevr_host::WaitForAttachFailureFlush(10000) ? 1 : 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  HMODULE exe = GetModuleHandleW(nullptr);
  g_record = exe == nullptr ? nullptr
                            : reinterpret_cast<RecordEventFn>(GetProcAddress(exe, kRecordEventExport));
  Record(kHostAttachEnter);

  const auto game_image = exe == nullptr ? nullptr
                                         : reinterpret_cast<GameImageFn>(
                                               GetProcAddress(exe, kGameImageExport));
  HMODULE game = game_image == nullptr ? nullptr : game_image();
  const auto no_event = exe == nullptr ? nullptr
                                       : reinterpret_cast<NoInstallEventFn>(
                                             GetProcAddress(exe, kNoInstallEventExport));
  if (no_event != nullptr && no_event() != 0) {
    nevr_host::RuntimeRendezvous::Seams().fail_event_create = true;
  }
  nevr_host::OnProcessAttach(module, game, false);

  if (nevr_host::Rendezvous().Installed()) {
    Record(kHostHookEnabled,
           *(reinterpret_cast<const unsigned char*>(game) + nevr_host::kGameMainWrapperRva));
  } else {
    Record(kHostHookFailed);
  }

  const auto hold = exe == nullptr ? nullptr
                                   : reinterpret_cast<AttachHoldFn>(
                                         GetProcAddress(exe, kAttachHoldExport));
  const DWORD hold_ms = hold == nullptr ? 0 : hold();
  if (hold_ms != 0) {
    Record(kHostAttachHeld, hold_ms);
    Sleep(hold_ms);
  }
  Record(kHostAttachExit, GetModuleHandleW(L"nevr.dll") != nullptr ? 1 : 0);
  return TRUE;  // the game statically imports this DLL; a FALSE would abort start
}
