#include "runtime/host/host_entry.h"

#include "runtime/host/crash_report_host.h"
#include "runtime/host/stop_line.h"
#include "runtime/lifecycle/game_image_guard.h"

#include <atomic>

namespace nevr_host {
namespace {

BootstrapDiagnostics g_diagnostics;
RuntimeRendezvous g_rendezvous;
CrashCapture g_capture;
HMODULE g_host_module = nullptr;
std::atomic<bool> g_set_game_module_called{false};
std::atomic<bool> g_attached_at_dll_main{false};
std::atomic<bool> g_not_attached_reported{false};
std::atomic<HANDLE> g_failure_flush_thread{nullptr};
std::atomic<HANDLE> g_dbghelp_preload_thread{nullptr};

// The one stop line of an attach that left the game without the runtime (stop_line.h), written once per run, after the
// journal is flushed and never under the loader lock: every caller is a thread or a launcher call, not DllMain.
void ReportNotAttachedOnce() noexcept {
  if (g_not_attached_reported.exchange(true)) return;
  ReportNotAttached(g_diagnostics);
}

DWORD WINAPI FlushAfterAttachFailure(LPVOID) {
  g_diagnostics.FlushAttachJournal();
  ReportNotAttachedOnce();
  return 0;
}

// An attach that left the game unhooked has no detour to report it, and the game may never construct the
// crash reporter either, so the record would never reach the disk. A file cannot be written under the loader
// lock, so the flush runs on a thread: its start waits for the lock, which DllMain releases on return.
void FlushJournalOffTheLoaderLock() noexcept {
  const HANDLE thread = CreateThread(nullptr, 0, &FlushAfterAttachFailure, nullptr, 0, nullptr);
  g_failure_flush_thread.store(thread, std::memory_order_release);
}

DWORD WINAPI PreloadDbgHelpThread(LPVOID) {
  g_capture.PreloadDbgHelp();
  return 0;
}

// The crash capture needs dbghelp.dll mapped before the fault (it never loads a library inside the crash
// callback), and nothing else loads it. DllMain holds the loader lock, so the load runs on a thread: its start
// waits for the lock, which DllMain releases on return. Started for both attach modes.
void StartDbgHelpPreload() noexcept {
  const HANDLE thread = CreateThread(nullptr, 0, &PreloadDbgHelpThread, nullptr, 0, nullptr);
  g_dbghelp_preload_thread.store(thread, std::memory_order_release);
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
  g_rendezvous.Prepare(g_host_module, game_module, g_diagnostics);
  g_rendezvous.Install(RendezvousTarget(game_module));
}

}  // namespace

void OnProcessAttach(HMODULE host_module, HMODULE game_module, bool launcher_mode) noexcept {
  // Under the loader lock no file is written: a flush that runs on this thread before it returns is a defect, and
  // BootstrapDiagnostics counts it (FlushesOnAttachThread).
  struct AttachThreadMark {
    AttachThreadMark() { BootstrapDiagnostics::MarkAttachThread(GetCurrentThreadId()); }
    ~AttachThreadMark() { BootstrapDiagnostics::MarkAttachThread(0); }
  } attach_thread_mark;
  g_host_module = host_module;
  // Fixed memory and one kernel32 call under the loader lock: the diagnostic and crash paths, and
  // the log mutex (created in Prepare, so no crash callback creates it). No directory creation, no
  // file I/O, no runtime load.
  g_diagnostics.Prepare(host_module);
  g_capture.Prepare(host_module, &g_diagnostics);
  PrepareCrashReport(&g_diagnostics);  // the file name and the platform: no file I/O and no module enumeration (loader lock)
  StartDbgHelpPreload();
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

bool WaitForDbgHelpPreload(DWORD timeout_ms) noexcept {
  const HANDLE thread = g_dbghelp_preload_thread.load(std::memory_order_acquire);
  if (thread == nullptr) return false;
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
    ReportUnsupportedImage(g_diagnostics, nevr_game_image_guard::kUnsupportedImageExitCode);
    nevr_game_image_guard::RequireSupportedGameModule(game_module);  // ends the process
    return;
  }
  InstallRendezvous(game_module);
  g_diagnostics.FlushAttachJournal();
  if (!g_rendezvous.Installed()) ReportNotAttachedOnce();  // a prologue or a hook that failed: the game runs without it
}

void CreateReport(PEXCEPTION_POINTERS exception_pointers) noexcept {
  // The readable report (#1689): written here only when the runtime is not there to write it. A loaded runtime's crash-first
  // filter has already written this crash's report before the game's crash reporter calls the host, so one crash is one
  // report; a crash with no runtime (absent, failed to load, not loaded yet) or in the host itself has this one. Before the
  // dump, because it is small and the dump is the step that can hang.
  if (g_rendezvous.RuntimeApi() == nullptr) WriteCrashReport(exception_pointers);
  g_capture.Capture(exception_pointers);
}

void OnCrashReporterConstructed() noexcept {
  g_diagnostics.FlushAttachJournal();
}

BootstrapDiagnostics& Diagnostics() noexcept { return g_diagnostics; }
RuntimeRendezvous& Rendezvous() noexcept { return g_rendezvous; }
CrashCapture& Capture() noexcept { return g_capture; }

}  // namespace nevr_host
