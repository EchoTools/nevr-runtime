#pragma once

// The Quest side of the crash report (core/crash_report.h, #1605): the platform the shared report asks for registers,
// backtrace, thread name, symbol, file and logcat line. The signal handler that owns the slot (src/quest/sentinel,
// sentinel.cpp) calls nevr_crash::WriteReport with a CrashInfo whose context is the handler's ucontext_t*.
//
// Runs in a signal handler: no allocation, no locks of ours, no sentinel log (its writer is not async-signal-safe). The one
// record the crash marker writes is a line of the run's own log file, appended with write(2) to a descriptor opened at arm time
// (sentinel_log.h, OpenCrashRecordFile); the marker writes nothing to logcat on the crash path.

#include <cstddef>

namespace sentinel {

// A report's or a minidump's path, with its NUL: the minidump path breakpad announces, the report beside it, and the buffer
// the dump callback hands FinishCrashReport.
inline constexpr std::size_t kReportPathBytes = 640;

// Registers the Quest platform with the shared report. Call once at arm time, before a crash can arrive.
void RegisterCrashReportPlatform();

// The breakpad dump callback's part, called with the minidump's path (`<dir>/<stem>.dmp`). The report belongs beside the
// minidump under the same stem (`<stem>` + nevr_crash::kReportSuffix), which is known only here:
//  - a handler already wrote the report (under a provisional stem, with the registers of the faulting thread): it is
//    renamed to the minidump's stem and the new path returned in `report_path`;
//  - none did: one is written now, without a register context (this callback is not given one).
// Returns false when no report exists afterwards. `report_path` (`capacity` bytes) is empty on false.
bool FinishCrashReport(const char* dump_path, char* report_path, unsigned long capacity);

// The self-update's crash marker (docs/adr/0024-pc-self-update.md, section 6). A crash that the game does not recover from, while
// update/pending.json exists, leaves update/crashed (the crash handler asks for it after the game's
// handler, crash_first_arm.h), so the next start rolls the pending release back. ArmUpdateCrashMarker builds the two paths
// once, from the nevr root (the folder that holds update/, NUL-terminated; nullptr or "" disarms the marker), and returns
// false when the root leaves no room for them. Call it at arm time, before a crash can arrive.
//
// It also opens the run's log file for the marker's record (OpenCrashRecordFile) and builds the record's fixed text. When the
// file cannot be opened, or the record does not fit its buffer, it writes one Warning to logcat, `[NEVR.HOST] crash_marker record
// unavailable reason=<record_too_long|log_not_opened> errno=<n>`, and the marker still arms: update/crashed is written, the
// crash path writes no record and does not try to open the file again. Arming again closes the earlier arm's descriptor.
bool ArmUpdateCrashMarker(const char* nevr_root);

// What MarkUpdateCrashed did. kNotArmed and kNoPending write nothing; kWritten and kFailed also append one `crash_marker` record to
// the run's log: `event` `crash_marker`, `tag` `NEVR.HOST`, and the header keys `ts` (UTC, ISO 8601 with milliseconds), `run` and
// `level` (`info` for kWritten, `warn` for kFailed), then `outcome` (`written` or `failed`), `path` (the crashed file), `thread`
// (the faulting thread's id) and, for kFailed only, `errno` (the open's, the POSIX counterpart of the PC record's `win32_error`).
enum class CrashMarkerOutcome : unsigned char { kNotArmed, kNoPending, kWritten, kFailed };

// The marker for the crash path: one access() of pending.json and, only when it is there, one open(O_CREAT) of the empty
// crashed, then the one record line. Async-signal-safe: fixed storage, raw syscalls (access, open, close, clock_gettime, gettid,
// write), errno saved and restored, no heap, no lock, no sentinel log, no logcat. The report action calls it first, so it runs
// whatever the report and the dump do.
CrashMarkerOutcome MarkUpdateCrashed() noexcept;

}  // namespace sentinel
