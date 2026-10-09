#pragma once

// One record of nevr-boot.jsonl: {"ts":"<UTC>","run":"<id>","level":"info","msg":"<escaped>"}\n
// Heap-free so the boot tee (which runs under the DllMain loader lock) can build it; the same
// header parses the file back for the replay into the main log (boot_replay.h).

#include <cstdio>

namespace BootLines {

/// Writes the record into `buf`. `ts` is an ISO 8601 UTC timestamp in the main log's format,
/// `escapedMsg` is already JSON-escaped. Returns the length, or -1 if it did not fit.
inline int Build(char* buf, int size, const char* ts, const char* runId, const char* escapedMsg) {
  const int n = std::snprintf(buf, static_cast<size_t>(size),
                              "{\"ts\":\"%s\",\"run\":\"%s\",\"level\":\"info\",\"msg\":\"%s\"}\n", ts, runId,
                              escapedMsg);
  return (n <= 0 || n >= size) ? -1 : n;
}

}  // namespace BootLines
