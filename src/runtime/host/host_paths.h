#pragma once

// Where the stable host keeps what it writes: in its own per-game folder, never in
// the game's own %LOCALAPPDATA%\EchoVR:
//   %LOCALAPPDATA%\nevr\echovr\logs\nevr-bootstrap.jsonl
//   %LOCALAPPDATA%\nevr\echovr\crashes\nevr-<pid>-<n>.dmp
// with %TEMP%\nevr\echovr\{logs,crashes} as the fallback and the host's own
// directory as the last resort.

namespace nevr_host {

inline constexpr wchar_t kLogsSubdirectory[] = L"nevr\\echovr\\logs";
inline constexpr wchar_t kCrashesSubdirectory[] = L"nevr\\echovr\\crashes";
inline constexpr wchar_t kBootstrapLogName[] = L"nevr-bootstrap.jsonl";

// One append to the log is serialized with this named mutex. WriteFile with FILE_APPEND_DATA is not
// atomic under Wine: concurrent appenders overwrite each other's lines, within a process and across
// two processes that share a prefix (a client and a local server). A named mutex covers both, is
// recursive for the thread that owns it (a crash inside an append can still write its record) and
// has a timed wait, so a holder that never releases never blocks a write: the line is written
// without the lock. The mutex is created once, by BootstrapDiagnostics::Prepare at attach, never by
// an append. Each append's wait is bounded by kBootstrapLogLockWaitMs. A wait that times out (or fails)
// marks the lock unreliable, and every later append in the process writes without waiting. A wait that
// ends with the lock acquired is not capped per process: a crash of up to four records can wait four
// times, each at most kBootstrapLogLockWaitMs, if a holder releases inside every wait.
inline constexpr wchar_t kBootstrapLogMutexName[] = L"Local\\nevr-host-bootstrap-log";
inline constexpr unsigned long kBootstrapLogLockWaitMs = 500;

}  // namespace nevr_host
