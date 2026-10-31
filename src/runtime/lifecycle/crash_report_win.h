#pragma once

#include <cstdint>

// The PC side of the crash report (core/crash_report.h, #1605): the platform the shared report asks for registers,
// backtrace, symbols, file and the summary line. The vectored handler in crash_recovery.cpp calls
// nevr_crash::WriteReport with a CrashInfo whose context is the handler's EXCEPTION_POINTERS*.
//
// Crash path (N70): raw WriteFile and reads of memory VirtualQuery says are mapped; no Log(), no allocation, no
// module enumeration. Modules come from the table crash_recovery.cpp snapshots at init.

namespace nevr_crash_win {

struct ModuleSpan {
  std::uint64_t base;
  std::uint64_t end;
  const char* name;  // basename, NUL-terminated, owned by the snapshot
};

// Finds the snapshot entry holding `address`; false when none does.
using ModuleLookupFn = bool (*)(std::uint64_t address, ModuleSpan* out);

// Registers the PC platform with the shared report. `crashes_dir` is the crash directory
// (`%LOCALAPPDATA%\nevr\echovr\crashes`); a report is written there as `nevr-crash-<run id>-<n>` plus
// nevr_crash::kReportSuffix, whole and atomically (a temporary name, renamed last). An empty directory leaves the
// report unwritten but still logged. Call once at init, before a crash can arrive; the directory must exist by then
// (the crash path creates nothing).
void RegisterCrashReportPlatform(const char* crashes_dir, ModuleLookupFn lookup);

}  // namespace nevr_crash_win
