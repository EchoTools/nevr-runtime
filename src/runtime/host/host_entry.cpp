#include "runtime/host/host_entry.h"

#include "runtime/lifecycle/game_image_guard.h"

#include <atomic>

namespace nevr_host {
namespace {

BootstrapDiagnostics g_diagnostics;
RuntimeRendezvous g_rendezvous;
CrashCapture g_capture;
HMODULE g_host_module = nullptr;
std::atomic<bool> g_set_game_module_called{false};

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
  g_host_module = host_module;
  // Fixed memory only under the loader lock: the diagnostic and crash paths. No
  // directory creation, no file I/O, no runtime load.
  g_diagnostics.Prepare(host_module);
  g_capture.Prepare(host_module, &g_diagnostics);
  if (launcher_mode) return;  // SetGameModule drives the rendezvous after LoadLibrary
  InstallRendezvous(game_module);
}

void SetGameModule(HMODULE game_module) noexcept {
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
