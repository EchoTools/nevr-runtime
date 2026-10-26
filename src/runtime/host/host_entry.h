#pragma once

// What the stable BugSplat64.dll host does behind its DllMain and its exports
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md). The DLL's entry points are
// one-line calls into these functions, so the tests drive the same code the game
// runs and not a copy of it.
//
//   OnProcessAttach           DllMain, DLL_PROCESS_ATTACH. Under the loader lock: fixed
//                             memory only, plus the one MinHook detour. Loads nothing.
//                             A refused image, prologue or hook is reported by a thread started here (a file cannot
//                             be written under the loader lock), so the host log says why the runtime is off.
//   OnProcessAttach also starts the thread that loads dbghelp.dll for the crash capture (nothing else does).
//   OnProcessDetach           DllMain, DLL_PROCESS_DETACH at process termination: writes the attach journal's
//                             remainder, so a process that exits at once still says why the runtime is off.
//   SetGameModule             NEVR_SetGameModule, the external launcher contract.
//   CreateReport              MiniDmpSender::createReport, the minidump path.
//   OnCrashReporterConstructed  MiniDmpSender::MiniDmpSender, the always-present trigger
//                             that reports attach decisions when no detour exists.

#include "runtime/host/bootstrap_diagnostics.h"
#include "runtime/host/crash_capture.h"
#include "runtime/host/runtime_rendezvous.h"

#include <windows.h>

namespace nevr_host {

/// `game_module` is the process image in direct-import mode. In launcher mode the
/// image arrives later through SetGameModule, so the host only prepares here.
void OnProcessAttach(HMODULE host_module, HMODULE game_module, bool launcher_mode) noexcept;

/// DllMain, DLL_PROCESS_DETACH. `process_terminating` is `lpReserved != NULL`. On termination, writes whatever the
/// attach journal still holds (see BootstrapDiagnostics::FlushAttachJournalAtExit); a dynamic unload does nothing.
void OnProcessDetach(bool process_terminating) noexcept;

/// After an attach that left the game unhooked (a refused image, prologue or hook), waits up to `timeout_ms` for
/// the thread that writes the attach journal to finish. True when it finished or none was started. The game never
/// needs this: it is for a test that must read the journal before its process ends.
bool WaitForAttachFailureFlush(DWORD timeout_ms) noexcept;

/// Waits up to `timeout_ms` for the thread OnProcessAttach starts to load dbghelp.dll for the crash capture. True when
/// that thread finished (whether or not the load succeeded: a failure is in the diagnostics log). The game never needs
/// this: a crash before the thread ran reports dbghelp_unavailable. It is for a test that must crash after the load.
bool WaitForDbgHelpPreload(DWORD timeout_ms) noexcept;

/// Validate the game image (ending the process with the stable image-guard exit
/// code on an unsupported one, after recording the refusal) and install the same
/// rendezvous direct-import mode installs. Not under the loader lock: pending attach
/// decisions are reported before it returns.
void SetGameModule(HMODULE game_module) noexcept;

/// The BugSplat_createReport body: write the minidump and the records about it.
void CreateReport(PEXCEPTION_POINTERS exception_pointers) noexcept;

/// Report the attach decisions recorded so far. Runs after attach has returned.
void OnCrashReporterConstructed() noexcept;

// The host's process-wide state, for the tests that observe it.
BootstrapDiagnostics& Diagnostics() noexcept;
RuntimeRendezvous& Rendezvous() noexcept;
CrashCapture& Capture() noexcept;

}  // namespace nevr_host
