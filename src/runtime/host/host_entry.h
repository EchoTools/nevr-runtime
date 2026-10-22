#pragma once

// What the stable BugSplat64.dll host does behind its DllMain and its exports
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md). The DLL's entry points are
// one-line calls into these functions, so the tests drive the same code the game
// runs and not a copy of it.
//
//   OnProcessAttach           DllMain, DLL_PROCESS_ATTACH. Under the loader lock: fixed
//                             memory only, plus the one MinHook detour. Loads nothing.
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
/// `in_process_runtime`, when set, is the entry point of a runtime compiled into this image
/// (the rendezvous then loads nothing from disk); it is kept for SetGameModule.
void OnProcessAttach(HMODULE host_module, HMODULE game_module, bool launcher_mode,
                     NevrGetRuntimeApiFn in_process_runtime = nullptr) noexcept;

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
