// Sentinel logging: every line goes to logcat (tag NEVR-Sentinel) and is appended to the on-disk
// log `nevr-sentinel.log` in the app's external files directory as one JSON object per line.
// Callers pass key and feature names only, never configured values.
#pragma once

#include "quest/sentinel/quest_config.h"

#include <cstddef>
#include <string>

#include <sys/types.h>

namespace sentinel {

// The app's external files directory (compile-time default, overridable at build time).
const char* FilesDir();

// Writes `message` to logcat and the on-disk log. Open failures and write failures are each
// reported once to logcat at error level; logcat keeps receiving every line.
void Emit(nevr_quest::LogLevel level, const std::string& message);

// Logcat only, no allocation and no exceptions: for failure paths that must not throw (out of
// memory in a catch block). `literal` must be a string literal.
void EmitFixed(nevr_quest::LogLevel level, const char* literal) noexcept;

// Closes the on-disk log; the next Emit reopens it. Failure reports already made are not repeated.
void CloseDiskLog();

using WriteFn = ssize_t (*)(int, const void*, std::size_t);

// Writes all `size` bytes, retrying short writes and EINTR. A zero-byte write counts as failure
// (`*err` = EIO). Returns false with `*err` set on failure. `*written` (optional) receives the
// number of bytes that reached the file either way.
bool WriteAll(int fd, const char* data, std::size_t size, int* err, WriteFn write,
              std::size_t* written = nullptr);

// Appends one record with a single WriteAll. If the previous record was torn (`*torn`), a newline
// is written first so the fragment stays on its own line. `*torn` is set only when a failed call
// left a partial record in the file; a failure that wrote nothing leaves it as it was.
bool WriteRecord(int fd, const std::string& line, bool* torn, int* err, WriteFn write);

// The on-disk log is rotated (renamed to a name that does not exist yet, never deleted) when it
// reaches this size: at open, and in-process once this run has written that much. A rename that
// fails is logged once to logcat and not attempted again for kOpenRetryMs; the log keeps growing
// meanwhile, and a rotation that later succeeds re-arms the report.
inline constexpr long long kMaxDiskLogBytes = 1024 * 1024;

// After a failed open the log is not tried again for this long (a missing or not-yet-mounted
// directory can appear later). That covers every open that fails, including a directory at the
// path (EISDIR) and a symlink (O_NOFOLLOW: ELOOP). Only a path that opens but is not a regular
// file (a FIFO or a device: fstat says so) is never retried. Intervals are measured on
// CLOCK_MONOTONIC; the wall clock is only for timestamps and rotated names.
inline constexpr long long kOpenRetryMs = 30 * 1000;

// Test seams: a replacement clock (null restores the real ones) and the number of open attempts.
// SetClockForTest replaces both clocks; SetWallClockForTest only the wall clock (timestamps and
// rotated names), so a test can step it while the interval clock runs on.
using NowFn = long long (*)();
void SetClockForTest(NowFn now);
void SetWallClockForTest(NowFn now);
// A replacement for rename(2) in the rotation (null restores it).
using RenameFn = int (*)(const char* from, const char* to);
void SetRenameForTest(RenameFn rename);
unsigned OpenAttemptsForTest();

}  // namespace sentinel
