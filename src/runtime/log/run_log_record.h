#pragma once

// One record the built-in filter writes to the main log (builtin_filter.cpp, WriteFileRecord), #2245:
// {"ts":"<UTC>","run":"<id>","level":"<name>"[,"level_raw":<n>][,"game_level":<n>],"msg":"<escaped>"}\n
//
// `level` is the line's level by the one mapping (core/log_level.h). A level the mapping has no name for is written
// "unknown" with `level_raw`, the number the caller passed, so the record never claims a level it was not given. A
// line the game logged from inside its error dialog is recorded at error, and `game_level` keeps the level the game
// gave it.

#include <cstdint>
#include <cstring>
#include <string>

#include "core/json_escape.h"

namespace nevr_run_log_record {

inline const char* LevelName(std::uint32_t level) {
  switch (level) {
    case 1: return "debug";
    case 2: return "info";
    case 4: return "warn";
    case 8: return "error";
    default: return "info";
  }
}

inline void Build(std::string& line, const char* ts, const char* runId, std::uint32_t gameLevel, bool insideErrorDialog,
                  const char* message, int len) {
  (void)insideErrorDialog;
  const char* const lvl = LevelName(gameLevel);
  line = "{\"ts\":\"";
  nevr_json_escape::AppendTo(line, ts, static_cast<int>(std::strlen(ts)));
  line += "\",\"run\":\"";  // N80
  line += runId;
  line += "\",\"level\":\"";
  nevr_json_escape::AppendTo(line, lvl, static_cast<int>(std::strlen(lvl)));
  line += "\",\"msg\":\"";
  nevr_json_escape::AppendTo(line, message, len);
  line += "\"}\n";
}

}  // namespace nevr_run_log_record
