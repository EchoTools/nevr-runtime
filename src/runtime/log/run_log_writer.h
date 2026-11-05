#pragma once

// The runtime's writer of the process's one main log (core/run_log_path.h): `<root>\logs\nevr-<process creation time>.jsonl`.
// The boot phase (boot_log_tee.cpp) and the built-in log filter (builtin_filter.cpp) both append through this one object, so their
// records are one stream in one file with no replay between two files (docs/standards/logging.md, "One log"; ADR 0027 section 5).
//
// Kernel32 calls only, no CRT file functions (N43: no CRT stream lock is taken): the boot phase runs before the game's logger exists.
// A record is one WriteFile under an in-process SRW lock and the named mutex the host also takes
// (nevr_run_log::kMutexName): WriteFile with FILE_APPEND_DATA is not atomic under Wine, and the host's crash-report sender appends
// from its own thread while the runtime logs (docs/adr/0008).
//
// There is no fallback place. When the logs folder cannot be made or the file cannot be opened, the first such failure is written to
// standard error with its Win32 error, every later record is counted as lost, and nothing is written beside the game or in %TEMP%
// (AGENTS.md, Logging, rule 4: losing a record is visible).

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/nevr_root.h"
#include "core/run_log_path.h"

namespace nevr_run_log_writer {

namespace detail {

struct State {
  SRWLOCK lock = SRWLOCK_INIT;
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mutex = nullptr;
  char dir[MAX_PATH] = {};   // the folder the file is in; empty: the default, <root>\logs
  char path[MAX_PATH] = {};  // the file, once resolved
  bool resolved = false;
  bool open_failure_reported = false;
  unsigned long long bytes = 0;
  unsigned long long lost_records = 0;
};

inline State& Get() {
  static State state;
  return state;
}

inline void Report(const char* what, const char* path, DWORD error) {
  char line[MAX_PATH + 160];
  const int n = std::snprintf(line, sizeof(line),
                              "[NEVR.LOG] error; %s path=%s win32_error=%lu (records of this run are lost until it is fixed; nothing is written "
                              "anywhere else)\n",
                              what, path[0] != '\0' ? path : "(unresolved)", static_cast<unsigned long>(error));
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (n > 0 && err != nullptr && err != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(err, line, static_cast<DWORD>(n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1), &written, nullptr);
  }
}

// Resolves the path (once) and makes the folder. Caller holds the lock.
inline bool Resolve(State& s) {
  if (s.resolved) return s.path[0] != '\0';
  s.resolved = true;
  std::string dir = s.dir[0] != '\0' ? std::string(s.dir) : nevr_root::LogsDir(nevr_root::RootA());
  if (dir.empty()) {
    Report("no run log folder: %LOCALAPPDATA% is unset or too long", "", ERROR_ENVVAR_NOT_FOUND);
    return false;
  }
  if (!nevr_root::EnsureDirectoryTree(dir)) {
    Report("cannot make the run log folder", dir.c_str(), GetLastError());
    return false;
  }
  const std::string path = nevr_run_log::PathForThisProcess(dir);
  if (path.empty() || path.size() >= sizeof(s.path)) {
    Report("the run log path does not fit", dir.c_str(), ERROR_BUFFER_OVERFLOW);
    return false;
  }
  std::memcpy(s.path, path.c_str(), path.size() + 1);
  return true;
}

// Opens the file for append. Caller holds the lock. FILE_SHARE_DELETE lets a rotation rename it; the host opens it for append too.
inline bool Open(State& s) {
  if (s.file != INVALID_HANDLE_VALUE) return true;
  if (!Resolve(s)) return false;
  s.file = CreateFileA(s.path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
  if (s.file == INVALID_HANDLE_VALUE) {
    if (!s.open_failure_reported) {
      s.open_failure_reported = true;
      Report("cannot open the run log", s.path, GetLastError());
    }
    return false;
  }
  LARGE_INTEGER size{};
  s.bytes = GetFileSizeEx(s.file, &size) ? static_cast<unsigned long long>(size.QuadPart) : 0;
  if (s.mutex == nullptr) s.mutex = CreateMutexW(nullptr, FALSE, nevr_run_log::kMutexName);
  return true;
}

inline constexpr DWORD kMutexWaitMs = 200;

}  // namespace detail

/// Appends one record (a complete line ending in '\n') to the main log. False when it could not be written; the failure is counted
/// and the first one is on standard error.
inline bool Append(const char* data, std::size_t length) {
  detail::State& s = detail::Get();
  AcquireSRWLockExclusive(&s.lock);
  bool ok = false;
  if (detail::Open(s)) {
    const DWORD waited = s.mutex != nullptr ? WaitForSingleObject(s.mutex, detail::kMutexWaitMs) : WAIT_FAILED;
    const bool owned = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    std::size_t done = 0;
    ok = true;
    while (done < length && ok) {
      DWORD chunk = 0;
      ok = WriteFile(s.file, data + done, static_cast<DWORD>(length - done), &chunk, nullptr) != FALSE && chunk != 0;
      done += chunk;
    }
    if (owned) ReleaseMutex(s.mutex);
    s.bytes += done;
    if (!ok && !s.open_failure_reported) {
      s.open_failure_reported = true;
      detail::Report("cannot write the run log", s.path, GetLastError());
    }
  }
  if (!ok) ++s.lost_records;
  ReleaseSRWLockExclusive(&s.lock);
  return ok;
}

/// The main log's full path, resolved on first use; "" when it could not be (the reason went to standard error). The pointer stays
/// valid until the process ends.
inline const char* Path() {
  detail::State& s = detail::Get();
  AcquireSRWLockExclusive(&s.lock);
  detail::Resolve(s);
  ReleaseSRWLockExclusive(&s.lock);
  return detail::Get().path;
}

/// Bytes in the file now (what it held when opened, plus what this process wrote).
inline unsigned long long Bytes() {
  detail::State& s = detail::Get();
  AcquireSRWLockExclusive(&s.lock);
  const unsigned long long bytes = s.bytes;
  ReleaseSRWLockExclusive(&s.lock);
  return bytes;
}

/// Records that could not be written since the process started.
inline unsigned long long LostRecords() {
  detail::State& s = detail::Get();
  AcquireSRWLockExclusive(&s.lock);
  const unsigned long long lost = s.lost_records;
  ReleaseSRWLockExclusive(&s.lock);
  return lost;
}

/// Puts the log in another folder (the filter's `file_dir` setting): the file is the same name there. Records already written stay
/// where they were written. An empty `dir` selects the default, <root>\logs.
inline void SetDirectory(const char* dir) {
  detail::State& s = detail::Get();
  AcquireSRWLockExclusive(&s.lock);
  if (s.file != INVALID_HANDLE_VALUE) CloseHandle(s.file);
  s.file = INVALID_HANDLE_VALUE;
  s.resolved = false;
  s.path[0] = '\0';
  s.open_failure_reported = false;
  std::snprintf(s.dir, sizeof(s.dir), "%s", dir != nullptr ? dir : "");
  ReleaseSRWLockExclusive(&s.lock);
}

/// Rotation: the file written so far is renamed to `<name>.<n>.jsonl` (the smallest n that is free) and a new file of the same name
/// is begun, so the process's log keeps one name that the host and the crash path append to. False, with the file left in place and
/// still being appended to, when the rename was refused (the host holds the file open for an instant: tried a few times).
inline bool Rotate() {
  detail::State& s = detail::Get();
  AcquireSRWLockExclusive(&s.lock);
  bool rotated = false;
  if (s.file != INVALID_HANDLE_VALUE && s.path[0] != '\0') {
    CloseHandle(s.file);
    s.file = INVALID_HANDLE_VALUE;
    std::string base(s.path);
    const std::size_t dot = base.rfind(".jsonl");
    if (dot != std::string::npos) base.resize(dot);
    for (int n = 1; n < 1000 && !rotated; ++n) {
      const std::string archive = base + "." + std::to_string(n) + ".jsonl";
      if (GetFileAttributesA(archive.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
      for (int attempt = 0; attempt < 5 && !rotated; ++attempt) {
        rotated = MoveFileExA(s.path, archive.c_str(), 0) != FALSE;
        if (!rotated) Sleep(20);
      }
      break;
    }
    if (!rotated) detail::Report("cannot rotate the run log; it keeps growing", s.path, GetLastError());
    detail::Open(s);
  }
  ReleaseSRWLockExclusive(&s.lock);
  return rotated;
}

/// Closes the file and forgets the resolved path, so the next Append resolves it again (shutdown, and a test that changes
/// %LOCALAPPDATA%).
inline void Close() {
  detail::State& s = detail::Get();
  AcquireSRWLockExclusive(&s.lock);
  if (s.file != INVALID_HANDLE_VALUE) CloseHandle(s.file);
  s.file = INVALID_HANDLE_VALUE;
  s.resolved = false;
  s.path[0] = '\0';
  s.dir[0] = '\0';
  s.open_failure_reported = false;
  s.bytes = 0;
  ReleaseSRWLockExclusive(&s.lock);
}

}  // namespace nevr_run_log_writer
