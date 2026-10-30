#pragma once

// The Quest side of the crash report (core/crash_report.h, #1605): the platform the shared report asks for registers,
// backtrace, thread name, symbol, file and logcat line. The signal handler that owns the slot (src/quest/sentinel,
// sentinel.cpp) calls nevr_crash::WriteReport with a CrashInfo whose context is the handler's ucontext_t*.
//
// Runs in a signal handler: no allocation, no locks of ours, no logger.

namespace sentinel {

// Registers the Quest platform with the shared report. Call once at arm time, before a crash can arrive.
void RegisterCrashReportPlatform();

// The breakpad dump callback's part, called with the minidump's path (`<dir>/<stem>.dmp`). The report belongs beside the
// minidump under the same stem (`<stem>` + nevr_crash::kReportSuffix), which is known only here:
//  - a handler already wrote the report (under a provisional stem, with the registers of the faulting thread): it is
//    renamed to the minidump's stem and the new path returned in `report_path`;
//  - none did: one is written now, without a register context (this callback is not given one).
// Returns false when no report exists afterwards. `report_path` (`capacity` bytes) is empty on false.
bool FinishCrashReport(const char* dump_path, char* report_path, unsigned long capacity);

}  // namespace sentinel
