#pragma once

// What the launcher shows of a server's own log (#1246): which files in the nevr logs folder are run logs, and how one record
// reads on the console. Pure: the folder listing is the launcher's Windows call, the record's lines are read by
// nevr_boot_replay::ReadNew (runtime/log/boot_replay.h), which keeps the records of one run and the offset to read from.

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/log/boot_replay.h"

namespace nevr_server_log {

/// A file in the nevr logs folder: its name and the time it was last written (FILETIME: 100 ns ticks since 1601).
struct LogFileInfo {
  std::string name;
  unsigned long long written = 0;
};

/// True for the runtime's run log names, nevr-<year>-<month>-...jsonl (BuildLogFilePath in runtime/log/builtin_filter.cpp).
/// The boot spool (nevr-boot.jsonl) and the host's log (nevr-bootstrap.jsonl) are not run logs.
inline bool IsRunLogName(std::string_view name) {
  constexpr std::string_view kPrefix = "nevr-";
  constexpr std::string_view kSuffix = ".jsonl";
  constexpr std::size_t kYearDigits = 4;
  if (name.size() < kPrefix.size() + kYearDigits + kSuffix.size()) return false;
  if (name.substr(0, kPrefix.size()) != kPrefix) return false;
  if (name.substr(name.size() - kSuffix.size()) != kSuffix) return false;
  for (std::size_t i = kPrefix.size(); i < kPrefix.size() + kYearDigits; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

/// The run log names among `files` written at or after `since`, in name order. The name carries the file's start time (UTC,
/// to the millisecond), so name order is start order: the newest run log is the last one.
inline std::vector<std::string> RunLogCandidates(const std::vector<LogFileInfo>& files, unsigned long long since) {
  std::vector<std::string> names;
  for (const LogFileInfo& file : files) {
    if (file.written >= since && IsRunLogName(file.name)) names.push_back(file.name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

/// The console line of one record, `<ts> <level> <msg>`, or "" for a record that is not shown: debug is off for an operator
/// (docs/standards/logging.md). Line breaks in the message are flattened, so one record is one line.
inline std::string RenderLine(const nevr_boot_replay::Line& line) {
  if (line.level == "debug") return std::string();
  std::string text = line.ts + " " + line.level + " " + line.msg;
  for (char& c : text) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  return text;
}

}  // namespace nevr_server_log
