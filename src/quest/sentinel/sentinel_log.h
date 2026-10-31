// Sentinel logging: every line goes to logcat (tag NEVR-Sentinel) and is appended to the on-disk
// log `nevr-sentinel.log` as one JSON object per line. The log lives in LogDir(), not in FilesDir():
// the app's Android/data folder may not exist on a headset, and the media folder is the one the
// player can reach. Callers pass key and feature names only, never configured values.
#pragma once

#include "quest/sentinel/quest_config.h"

#include <cstddef>
#include <string>

#include <sys/types.h>

namespace sentinel {

// The app's external files directory (compile-time default, overridable at build time). The sentinel
// config (config.yaml) and the hardware dump live here; the log does not (LogDir).
const char* FilesDir();

// One place the on-disk log may live. `dir` is the folder; `trusted` is a leading part of `dir` (up to a
// '/' or the whole path) that is never created or checked and may be a link (/sdcard is one on a
// headset). Every folder of `dir` below `trusted` is created if missing (mkdir -p), and each one must be
// a real directory: a link below `trusted` is refused, never followed. `name` is the token the Info
// line carries.
struct LogDirCandidate {
  const char* name;
  const char* dir;
  const char* trusted;
};

// mkdir -p of `c.dir` below `c.trusted`, one folder at a time: each must be a directory by lstat, so a link below the
// trusted part is refused (errno ENOTDIR), never followed. A `dir` not under `trusted` (up to a '/'), or longer than
// 400 bytes, is refused with EINVAL. True when every folder below the trusted part is there; whether the process may
// write into it is the caller's to find out (the log opens its file, the crash folder checks access). On false errno
// says why. Also the chooser of the crash folder (crash_dir.h).
bool EnsureDirBelow(const LogDirCandidate& c);

// The folder the on-disk log and its rotated files are in, for everything that reads them (the log
// upload). It is chosen once, on the first open that works: the first candidate whose folder can be
// made and whose log file can be opened. Candidates, in order: with NEVR_QUEST_FILES_DIR defined at
// build time (the host tests) only that folder, which is never created; otherwise the app's
// Android/media folder (`nevr_quest::data_root::kMediaRoot`, created below
// Android/media/com.readyatdawn.r15) then the Android/data files folder (created below Android/data).
// Before any open has worked it is the first candidate's folder. If the folder vanishes the next open
// chooses again. Each choice is told in the log (logcat at once, the file when it is open): one Warning per
// candidate that could not be used, event `sentinel_log_dir_failed` with its `dir`, `errno` and `error` (not
// repeated while it keeps failing the same way), then one Info, event `sentinel_log_dir`, for the winner.
std::string LogDir();

// The log of the session before this one, kept whole (the file `nevr-sentinel.session-<unix ms>.log` in LogDir() with
// the highest time, then the highest collision number), or "" when there is none. The first open of a process renames
// the log a previous process left to such a name, so the log upload reads two files: LogDir()/nevr-sentinel.log (this
// session) and this one. A session that ran past kMaxDiskLogBytes also left size-rotated files beside it
// (`nevr-sentinel.<unix ms>.log`); those are not named here and are never deleted.
std::string PreviousSessionLog();

// Test seam: the next open behaves as the first open of a new process (it keeps the log that is there).
void ResetSessionForTest();

// One line of the on-disk log: a single JSON object ending in '\n', no ANSI, valid UTF-8 (invalid bytes in
// `message` are replaced). A `message` that is itself a JSON object is flattened into the line: its fields stay
// fields, its `ts_ms` is dropped, its `level` is kept (else `level` names `level`); any other message becomes `msg`.
std::string FormatDiskLogLine(nevr_quest::LogLevel level, long long unixMs, const std::string& message);

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

// The on-disk log is kept by session (see PreviousSessionLog) and is also rotated (renamed to a name that does not exist
// yet, never deleted) when it reaches this size: at open, and in-process once this run has written that much. A rename that
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
// Replaces the candidate list (null or 0 restores the built-in one) and forgets the chosen folder. The
// strings must outlive the use.
void SetLogDirCandidatesForTest(const LogDirCandidate* list, std::size_t count);
// The list the shipped build uses (the host tests build with only their own folder, so they read it here).
const LogDirCandidate* ShippedLogDirCandidatesForTest(std::size_t* count);

}  // namespace sentinel
