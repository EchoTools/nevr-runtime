#pragma once

// The host's own crash report (#1689): the readable report (core/crash_report.h) for a crash that nevr-runtime.dll is not
// there to report. BugSplat64.dll receives every crash of the process through createReport; when the runtime is loaded its
// crash-first filter has already written the report (same file format, runtime/lifecycle/crash_report_win.cpp) and the host
// writes none, so one crash is one report. When the runtime is absent, failed to load or has not loaded yet, or the crash
// is in the host itself, the host writes it, through the same shared writer and the same PC pieces
// (runtime/lifecycle/crash_report_win_common.h), into the same folder, so the next start's sender (core/crash_send.h) sends it.
//
// Crash path (N70): fixed buffers, raw kernel32 file calls, no allocation, no lock, no logger. The module table is a
// snapshot taken before the crash (SnapshotModulesForCrashReport), never an enumeration in the handler.

#include "runtime/host/bootstrap_diagnostics.h"

#include <windows.h>

namespace nevr_host {

/// At attach, after CrashCapture::Prepare: registers the PC platform with the shared report and builds the report's file name
/// from %LOCALAPPDATA% (`<root>\crashes\nevr-host-<pid>-<time>-<n>.report.txt`). No file I/O, no module enumeration, so it is
/// safe under the loader lock. `diagnostics` (must outlive the process) receives the one `crash_report` record per report.
void PrepareCrashReport(BootstrapDiagnostics* diagnostics) noexcept;

/// Takes the snapshot of the modules mapped now (name, range, PE identity) the report resolves addresses against. Call it
/// once, from a thread that does not hold the loader lock (the detour on the game's main wrapper). A crash before it
/// still gets a report, with raw addresses.
void SnapshotModulesForCrashReport() noexcept;

/// The createReport body's report: writes the report for `exception_pointers` and returns true when the whole file was
/// written. Creates the crash folder if it is missing. A second call while one runs returns false at once.
bool WriteCrashReport(PEXCEPTION_POINTERS exception_pointers) noexcept;

}  // namespace nevr_host
