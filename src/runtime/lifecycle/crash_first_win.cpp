#include "runtime/lifecycle/crash_first_win.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "core/crash_report.h"
#include "core/logging.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/crash_first_filter.h"
#include "runtime/lifecycle/crash_first_report.h"
#include "runtime/lifecycle/crash_recovery.h"

namespace {

typedef LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* SetUnhandledExceptionFilterFunc)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetUnhandledExceptionFilterFunc OriginalSetUnhandledExceptionFilter = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_firstRequestLogged{false};

// The game's SetUnhandledExceptionFilter. The request is kept and the call is not passed on, so our filter stays installed.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilterHook(LPTOP_LEVEL_EXCEPTION_FILTER requested) {
  const LPTOP_LEVEL_EXCEPTION_FILTER previous = nevr_crash_first_win::RememberGameFilter(requested);
  if (!g_firstRequestLogged.exchange(true, std::memory_order_acq_rel)) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.CRASH] game crash filter request kept instead of installed filter=%s so=our top-level filter stays first "
        "and runs the game's after it",
        requested != nullptr ? "set" : "cleared");
  }
  return previous;
}

// Ours, for the first fatal exception: the shared report. BreakpointVEH's WriteCrashDump writes one for a fault in the game
// or a NEVR module before it reaches here; a second report for the same crash is not written.
void ReportFirstFatal(EXCEPTION_POINTERS* info) noexcept {
  if (nevr_crash::ReportWritten()) return;
  nevr_crash::WriteReport(nevr_crash_first_win::BuildFirstFatalInfo(info, GetCurrentThreadId()));
}

// One crash-record line after the game's filter returned: whether the game had a filter of its own to run after ours, and how
// many times it asked to install one (each would have displaced ours). Raw WriteFile only (N70).
void NoteChain(EXCEPTION_POINTERS* info, bool chained, long requests) noexcept {
  char line[nevr_crash_first_win::kRecordLineSize];
  nevr_crash_first_win::FormatFirstHandlerLine(line, sizeof(line), info, chained, requests);
  WriteCrashRecordLine(line);
}

}  // namespace

bool InstallCrashFirstFilter() {
  if (g_installed.exchange(true, std::memory_order_acq_rel)) return true;
  HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
  FARPROC target = kernel32 != nullptr ? GetProcAddress(kernel32, "SetUnhandledExceptionFilter") : nullptr;
  if (target == nullptr) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.CRASH] first handler NOT installed reason=SetUnhandledExceptionFilter_not_found error=%lu — the game's crash "
        "filter will replace ours",
        GetLastError());
    return false;
  }
  OriginalSetUnhandledExceptionFilter = reinterpret_cast<SetUnhandledExceptionFilterFunc>(target);
  nevr_crash_first_win::SetActions(&ReportFirstFatal, &NoteChain);
  if (!PatchDetour(&OriginalSetUnhandledExceptionFilter, reinterpret_cast<PVOID>(SetUnhandledExceptionFilterHook),
                   "SetUnhandledExceptionFilter")) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.CRASH] first handler NOT installed reason=hook_failed name=SetUnhandledExceptionFilter — the game's crash "
        "filter will replace ours");
    return false;
  }
  // The filter that was installed before ours becomes the game's previous one and runs after ours.
  const LPTOP_LEVEL_EXCEPTION_FILTER prior = OriginalSetUnhandledExceptionFilter(&nevr_crash_first_win::TopLevelFilter);
  nevr_crash_first_win::SeedPrior(prior);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.CRASH] first handler installed hook=SetUnhandledExceptionFilter prior_filter=%s so=our top-level filter runs "
      "first on an unhandled exception, then the game's",
      prior != nullptr ? "present" : "none");
  return true;
}
