#pragma once

// The per-process run ID (N80): PID and process creation time in hex, `<pid>-<creation>`. Every record the runtime writes
// carries it (`"run"` in each JSONL line), and so does the launcher that starts the game, which computes the same value
// for the child it created to find that child's log (src/launcher/echovr_server_launcher.cpp). One format, here.
//
// Header-only and free of writable data: the pure function takes its values as arguments, the Windows wrapper reads
// the process it is given and nothing else.

#include <cstddef>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace nevr_run_id {

/// Writes `<pid hex>-<creation hex>` into `out` (NUL-terminated, never past `size`).
inline void Format(unsigned long pid, unsigned long long creation, char* out, std::size_t size) {
  std::snprintf(out, size, "%lx-%llx", pid, creation);
}

#ifdef _WIN32
/// The creation time of `process` as a FILETIME value (100 ns ticks since 1601). 0 when the handle cannot be read.
inline unsigned long long CreationTime(HANDLE process) {
  FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
  if (!GetProcessTimes(process, &createTime, &exitTime, &kernelTime, &userTime)) return 0;
  return (static_cast<unsigned long long>(createTime.dwHighDateTime) << 32) | createTime.dwLowDateTime;
}
#endif

}  // namespace nevr_run_id
