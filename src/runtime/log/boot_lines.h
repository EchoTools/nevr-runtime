#pragma once

// One record of nevr-boot.jsonl: {"ts":"<UTC>","run":"<id>","level":"<level>","msg":"<escaped>"}\n
// The level is the one the message states after its tag (logging.md Rule 12: `[NEVR.X] warn; ...`), so a
// boot line can be found by level (#664). A message with no level word is `info`.
// Heap-free so the boot tee (which runs under the DllMain loader lock) can build it; the same
// header parses the file back for the replay into the main log (boot_replay.h).

#include <cstdio>
#include <cstring>

namespace nevr_boot_lines {

/// The level a boot message states: `debug`, `info`, `warn` or `error`, written as the first word after an
/// optional `[TAG] ` and followed by "; " (Rule 12). A message that states none, or a word that is not one of
/// the four, is `info`, the level a bare bootstrap line has always carried.
inline const char* LevelOf(const char* msg) {
  if (msg == nullptr) return "info";
  const char* p = msg;
  if (p[0] == '[') {
    const char* close = std::strchr(p, ']');
    if (close != nullptr && close[1] == ' ') p = close + 2;
  }
  static const char* const kLevels[] = {"info", "warn", "error", "debug"};
  for (const char* level : kLevels) {
    const size_t n = std::strlen(level);
    if (std::strncmp(p, level, n) == 0 && p[n] == ';' && p[n + 1] == ' ') return level;
  }
  return "info";
}

/// Writes the record into `buf`. `ts` is an ISO 8601 UTC timestamp in the main log's format,
/// `escapedMsg` is already JSON-escaped. Returns the length, or -1 if it did not fit.
inline int Build(char* buf, int size, const char* ts, const char* runId, const char* escapedMsg) {
  const int n = std::snprintf(buf, static_cast<size_t>(size),
                              "{\"ts\":\"%s\",\"run\":\"%s\",\"level\":\"%s\",\"msg\":\"%s\"}\n", ts, runId,
                              LevelOf(escapedMsg), escapedMsg);
  return (n <= 0 || n >= size) ? -1 : n;
}

}  // namespace nevr_boot_lines
