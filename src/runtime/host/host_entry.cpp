#include "runtime/host/host_entry.h"

#include "runtime/lifecycle/game_image_guard.h"

#include <atomic>

namespace nevr_host {
namespace {

BootstrapDiagnostics g_diagnostics;
RuntimeRendezvous g_rendezvous;
CrashCapture g_capture;
HMODULE g_host_module = nullptr;
NevrGetRuntimeApiFn g_in_process_runtime = nullptr;
std::atomic<bool> g_set_game_module_called{false};
std::atomic<bool> g_attached_at_dll_main{false};
std::atomic<HANDLE> g_failure_flush_thread{nullptr};

DWORD WINAPI FlushAfterAttachFailure(LPVOID) {
  g_diagnostics.FlushAttachJournal();
  return 0;
}

// An attach that left the game unhooked has no detour to report it, and the game may never construct the
// crash reporter either, so the record would never reach the disk. A file cannot be written under the loader
// lock, so the flush runs on a thread: its start waits for the lock, which DllMain releases on return.
void FlushJournalOffTheLoaderLock() noexcept {
  const HANDLE thread = CreateThread(nullptr, 0, &FlushAfterAttachFailure, nullptr, 0, nullptr);
  g_failure_flush_thread.store(thread, std::memory_order_release);
}

void* RendezvousTarget(HMODULE game_module) {
  return reinterpret_cast<unsigned char*>(game_module) + kGameMainWrapperRva;
}

// Validate the image, then install the one detour. Every decision is recorded in
// the diagnostics journal; nothing is written here. A failure leaves the game
// unhooked and returns normally: the game statically imports this DLL, so a FALSE
// return from DllMain would abort process start.
void InstallRendezvous(HMODULE game_module) noexcept {
  if (!nevr_game_image_guard::IsSupportedGameModule(game_module)) {
    g_diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kRejected);
    return;
  }
  g_diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  g_rendezvous.Prepare(g_host_module, game_module, g_diagnostics, g_in_process_runtime);
  g_rendezvous.Install(RendezvousTarget(game_module));
}

}  // namespace

void OnProcessAttach(HMODULE host_module, HMODULE game_module, bool launcher_mode,
                     NevrGetRuntimeApiFn in_process_runtime) noexcept {
  // Under the loader lock no file is written: a flush that runs on this thread before it returns is a defect, and
  // BootstrapDiagnostics counts it (FlushesOnAttachThread).
  struct AttachThreadMark {
    AttachThreadMark() { BootstrapDiagnostics::MarkAttachThread(GetCurrentThreadId()); }
    ~AttachThreadMark() { BootstrapDiagnostics::MarkAttachThread(0); }
  } attach_thread_mark;
  g_host_module = host_module;
  g_in_process_runtime = in_process_runtime;
  // Fixed memory and one kernel32 call under the loader lock: the diagnostic and crash paths, and
  // the log mutex (created in Prepare, so no crash callback creates it). No directory creation, no
  // file I/O, no runtime load.
  g_diagnostics.Prepare(host_module);
  g_capture.Prepare(host_module, &g_diagnostics);
  if (launcher_mode) return;  // SetGameModule drives the rendezvous after LoadLibrary
  g_attached_at_dll_main.store(true);  // the process image is checked (and hooked, or refused) here
  InstallRendezvous(game_module);
  if (!g_rendezvous.Installed()) FlushJournalOffTheLoaderLock();  // refused image, prologue or hook: say why
}

void OnProcessDetach(bool process_terminating) noexcept {
  if (!process_terminating) return;
  // The process is ending: the thread that flushes an attach failure may have been killed before it wrote, and
  // no later trigger will come (a game that exits at once never constructs its crash reporter). This is the last
  // chance to say why the runtime is off. Every other thread is dead, so the write needs no loader-lock
  // reasoning beyond kernel32 file calls; the flush takes no wait.
  g_diagnostics.FlushAttachJournalAtExit();
}

bool WaitForAttachFailureFlush(DWORD timeout_ms) noexcept {
  const HANDLE thread = g_failure_flush_thread.load(std::memory_order_acquire);
  if (thread == nullptr) return true;
  return WaitForSingleObject(thread, timeout_ms) == WAIT_OBJECT_0;
}

void SetGameModule(HMODULE game_module) noexcept {
  // After a direct-import attach the image was already checked: a call now would prepare the host
  // again and journal the image check a second time.
  if (g_attached_at_dll_main.load()) {
    g_diagnostics.RecordAttach(AttachStep::kSetGameModule, AttachOutcome::kRejected, 0, nullptr,
                               HookStage::kNone, AttachNote::kSetGameModuleAfterAttach);
    g_diagnostics.FlushAttachJournal();
    return;
  }
  // Once only: a second call would prepare the host again and journal the image check twice.
  if (g_set_game_module_called.exchange(true)) {
    g_diagnostics.RecordAttach(AttachStep::kSetGameModule, AttachOutcome::kRejected, 0, nullptr,
                               HookStage::kNone, AttachNote::kDuplicateSetGameModule);
    g_diagnostics.FlushAttachJournal();
    return;
  }
  if (!nevr_game_image_guard::IsSupportedGameModule(game_module)) {
    g_diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kRejected);
    g_diagnostics.FlushAttachJournal();
    nevr_game_image_guard::RequireSupportedGameModule(game_module);  // ends the process
    return;
  }
  InstallRendezvous(game_module);
  g_diagnostics.FlushAttachJournal();
}

void CreateReport(PEXCEPTION_POINTERS exception_pointers) noexcept {
  g_capture.Capture(exception_pointers);
}

void OnCrashReporterConstructed() noexcept {
  g_diagnostics.FlushAttachJournal();
}

BootstrapDiagnostics& Diagnostics() noexcept { return g_diagnostics; }
RuntimeRendezvous& Rendezvous() noexcept { return g_rendezvous; }
CrashCapture& Capture() noexcept { return g_capture; }

}  // namespace nevr_host
