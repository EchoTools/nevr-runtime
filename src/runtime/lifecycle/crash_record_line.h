#pragma once

// One crash line as one JSON record of the process's main log (core/run_log_path.h; docs/standards/logging.md, "One log"):
//   {"ts":"<UTC>","run":"<run id>","level":"error","tag":"NEVR.CRASH","event":"crash_record","msg":"<the line>"}\n
// Pure and heap-free: the crash path (BreakpointVEH, WriteCrashDump; N70) builds it in a caller buffer and writes it with one WriteFile.

#include <cstddef>
#include <cstdio>

#include "core/json_escape.h"

namespace nevr_crash_recovery {

/// The record for `line` (its trailing newline is dropped; control characters are escaped, so a record is always one line). Returns
/// the record's length including its newline, or 0 when it does not fit `out`. A line longer than 1199 bytes is cut at that length.
inline std::size_t BuildCrashRecord(const char* line, std::size_t length, const char* ts, const char* runId, char* out,
                                    std::size_t outSize) {
  while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) --length;
  char escaped[1200];
  nevr_json_escape::Into(line, static_cast<int>(length), escaped, static_cast<int>(sizeof(escaped)));
  const int n = std::snprintf(out, outSize,
                              "{\"ts\":\"%s\",\"run\":\"%s\",\"level\":\"error\",\"tag\":\"NEVR.CRASH\","
                              "\"event\":\"crash_record\",\"msg\":\"%s\"}\n",
                              ts, runId, escaped);
  return (n > 0 && static_cast<std::size_t>(n) < outSize) ? static_cast<std::size_t>(n) : 0;
}

}  // namespace nevr_crash_recovery
