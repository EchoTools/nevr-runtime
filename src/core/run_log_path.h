// run_log_path.h: the one file every writer of a PC process records into (docs/standards/logging.md, "One log"; ADR 0027 section 5).
//
// A process has one main JSON log: `<root>\logs\nevr-<UTC creation time of the process>.jsonl`, the root being
// %LOCALAPPDATA%\nevr\echovr (core/nevr_root.h). The host (BugSplat64.dll), the runtime's boot phase, the runtime's log filter and the
// crash path each write their records into that file, so a reader of one run reads one file. The name comes from the process's
// creation time, the value the run id (N80) is built from, so the host and the runtime, which are two images with no shared data,
// name the same file independently, in any order, with no hand-over.
//
// There is no other place. A writer that cannot make the logs folder or open the file reports that on standard error, and loses
// nothing silently; it never writes beside the game, in %TEMP%, or anywhere else (AGENTS.md, Logging, rule 4).
//
// Header-only and free of writable data (the host links no core state, tools/verify_host_no_core_state.py). The pure functions take
// the creation time and folder as arguments so a test supplies its own.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

#include "core/nevr_root.h"
#include "core/run_id.h"

namespace nevr_run_log {

// The named mutex that orders appends to the file across the images and processes that write it. WriteFile with FILE_APPEND_DATA is
// not atomic under Wine, and the host's crash-report sender writes while the runtime does (docs/adr/0008).
inline constexpr wchar_t kMutexName[] = L"Local\\nevr-host-bootstrap-log";

// FILETIME ticks (100 ns since 1601) to `YYYY-MM-DDTHH-MM-SS.mmm`, UTC. False when `ticks` is 0 or does not fit `size`.
template <typename C>
bool FormatTimestamp(unsigned long long ticks, C* out, std::size_t size) {
  if (ticks == 0 || size < 24) return false;
  FILETIME ft{};
  ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFull);
  ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  SYSTEMTIME st{};
  if (!FileTimeToSystemTime(&ft, &st)) return false;
  const unsigned fields[] = {st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds};
  const unsigned widths[] = {4, 2, 2, 2, 2, 2, 3};
  const char separators[] = {'-', '-', 'T', '-', '-', '.', '\0'};
  std::size_t at = 0;
  for (int i = 0; i < 7; ++i) {
    unsigned divisor = 1;
    for (unsigned w = 1; w < widths[i]; ++w) divisor *= 10;
    for (unsigned w = 0; w < widths[i]; ++w) {
      out[at++] = static_cast<C>('0' + (fields[i] / divisor) % 10);
      divisor /= 10;
    }
    if (separators[i] != '\0') out[at++] = static_cast<C>(separators[i]);
  }
  out[at] = static_cast<C>('\0');
  return true;
}

// `nevr-<timestamp>.jsonl` for a process created at `creation`; when the creation time cannot be read, `nevr-pid<pid hex>.jsonl`
// (still the same name in every image of the process).
template <typename C>
bool FileName(unsigned long long creation, unsigned long pid, C* out, std::size_t size) {
  C stamp[32] = {};
  const bool stamped = FormatTimestamp(creation, stamp, sizeof(stamp) / sizeof(stamp[0]));
  const char* prefix = "nevr-";
  std::size_t at = 0;
  auto put = [&](char c) {
    if (at + 1 >= size) return false;
    out[at++] = static_cast<C>(c);
    return true;
  };
  for (const char* p = prefix; *p != '\0'; ++p) {
    if (!put(*p)) return false;
  }
  if (stamped) {
    for (std::size_t i = 0; stamp[i] != static_cast<C>('\0'); ++i) {
      if (!put(static_cast<char>(stamp[i]))) return false;
    }
  } else {
    if (!put('p') || !put('i') || !put('d')) return false;
    char digits[24] = {};
    std::snprintf(digits, sizeof(digits), "%lx", pid);
    for (const char* p = digits; *p != '\0'; ++p) {
      if (!put(*p)) return false;
    }
  }
  for (const char* p = ".jsonl"; *p != '\0'; ++p) {
    if (!put(*p)) return false;
  }
  out[at] = static_cast<C>('\0');
  return true;
}

// The file's full path: `<logs folder>\<FileName>`. Empty when the logs folder is empty (no root) or the path does not fit.
template <typename C>
std::basic_string<C> PathIn(const std::basic_string<C>& logs_dir, unsigned long long creation, unsigned long pid) {
  if (logs_dir.empty()) return std::basic_string<C>();
  C name[64] = {};
  if (!FileName(creation, pid, name, sizeof(name) / sizeof(name[0]))) return std::basic_string<C>();
  return nevr_root::Join(logs_dir, name);
}

// The running process's own log under the folder `logs_dir`.
template <typename C>
std::basic_string<C> PathForThisProcess(const std::basic_string<C>& logs_dir) {
  return PathIn(logs_dir, nevr_run_id::CreationTime(GetCurrentProcess()), GetCurrentProcessId());
}

}  // namespace nevr_run_log
