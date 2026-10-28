#pragma once

#include "core/nevr_root.h"

// Where the stable host keeps what it writes: in its own per-game folder, never in
// the game's own %LOCALAPPDATA%\EchoVR:
//   %LOCALAPPDATA%\nevr\echovr\logs\nevr-bootstrap.jsonl
//   %LOCALAPPDATA%\nevr\echovr\crashes\nevr-<pid>-<n>.dmp
// with %TEMP%\nevr\echovr\{logs,crashes} as the fallback and the host's own
// directory as the last resort.

namespace nevr_host {

inline constexpr wchar_t kLogsSubdirectory[] = L"nevr\\echovr\\logs";
inline constexpr wchar_t kCrashesSubdirectory[] = L"nevr\\echovr\\crashes";

// The runtime resolves the same folders through core/nevr_root.h (#857). The host keeps its literals (it links no core code),
// and these checks make a drift between the two a compile error: each literal is <root>\<child> in nevr_root's names.
namespace host_paths_detail {
constexpr bool IsRootChild(const wchar_t* path, const wchar_t* child) {
  const wchar_t* root = nevr_root::Names<wchar_t>::kRoot;
  std::size_t i = 0;
  for (; root[i] != L'\0'; ++i) {
    if (path[i] != root[i]) return false;
  }
  if (path[i] != L'\\') return false;
  ++i;
  std::size_t j = 0;
  for (; child[j] != L'\0'; ++j) {
    if (path[i + j] != child[j]) return false;
  }
  return path[i + j] == L'\0';
}
static_assert(IsRootChild(kLogsSubdirectory, nevr_root::Names<wchar_t>::kLogs), "host logs folder differs from nevr_root");
static_assert(IsRootChild(kCrashesSubdirectory, nevr_root::Names<wchar_t>::kCrashes), "host crashes folder differs from nevr_root");
}  // namespace host_paths_detail

inline constexpr wchar_t kBootstrapLogName[] = L"nevr-bootstrap.jsonl";

// One append to the log is serialized with this named mutex. WriteFile with FILE_APPEND_DATA is not
// atomic under Wine: concurrent appenders overwrite each other's lines, within a process and across
// two processes that share a prefix (a client and a local server). A named mutex covers both, is
// recursive for the thread that owns it (a crash inside an append can still write its record) and
// has a timed wait, so a holder that never releases never blocks a write: the line is written
// without the lock. The mutex is created once, by BootstrapDiagnostics::Prepare at attach, never by
// an append. Each append's wait is bounded by kBootstrapLogLockWaitMs. A wait that times out (or fails)
// marks the lock unreliable, and every later append in the process writes without waiting. A wait that
// ends with the lock acquired is not capped per process: if a holder releases inside every wait, every
// destination an append opens waits once (at most kBootstrapLogLockWaitMs), and a record whose write
// fails after its wait falls back to the next of up to three destinations and waits again.
inline constexpr wchar_t kBootstrapLogMutexName[] = L"Local\\nevr-host-bootstrap-log";
inline constexpr unsigned long kBootstrapLogLockWaitMs = 500;

// The last flush of a process that is ending waits this long for the log mutex, once, and writes nothing when it
// does not get it (a live process on the same prefix may hold it).
inline constexpr unsigned long kBootstrapExitLogLockWaitMs = 250;

}  // namespace nevr_host
