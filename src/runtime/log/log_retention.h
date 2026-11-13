#pragma once

// #1296: which run logs the built-in filter deletes. The rule is journald's: keep everything, and delete the oldest
// files only while the logs folder is over its size cap. The open file is never deleted, and a cap of 0 means no cap.
//
// Header only and free of Win32, so the unit test drives the same function the filter calls (builtin_filter.cpp,
// PruneOldLogs). The caller lists the run log files of the folder with their sizes; the names are the timestamped
// nevr-*.jsonl names, so a sort by path is oldest first.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace nevr_log_retention {

inline constexpr uint64_t kBytesPerMiB = 1024ull * 1024ull;

struct RunLogFile {
  std::string path;
  uint64_t bytes = 0;
};

// The paths to delete, oldest first, so that what is left in the folder is at most cap_bytes. The file at open_path is
// counted toward the total but never returned. A cap of 0 returns nothing.
inline std::vector<std::string> SelectLogsToDelete(std::vector<RunLogFile> files, uint64_t cap_bytes,
                                                   const std::string& open_path) {
  std::vector<std::string> doomed;
  if (cap_bytes == 0) return doomed;

  std::sort(files.begin(), files.end(),
            [](const RunLogFile& a, const RunLogFile& b) { return a.path < b.path; });

  uint64_t total = 0;
  for (const RunLogFile& f : files) total += f.bytes;

  for (const RunLogFile& f : files) {
    if (total <= cap_bytes) break;
    if (f.path == open_path) continue;
    doomed.push_back(f.path);
    total -= f.bytes;
  }
  return doomed;
}

}  // namespace nevr_log_retention
