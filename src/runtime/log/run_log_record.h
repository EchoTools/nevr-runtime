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
#include "core/log_level.h"

namespace nevr_run_log_record {

inline void Build(std::string& line, const char* ts, const char* runId, std::uint32_t gameLevel, bool insideErrorDialog,
                  const char* message, int len) {
  const std::uint32_t level = nevr_log_level::Effective(gameLevel, insideErrorDialog);
  const char* const name = nevr_log_level::Name(level);
  line = "{\"ts\":\"";
  nevr_json_escape::AppendTo(line, ts, static_cast<int>(std::strlen(ts)));
  line += "\",\"run\":\"";  // N80
  line += runId;
  line += "\",\"level\":\"";
  line += name != nullptr ? name : "unknown";
  line += '"';
  if (name == nullptr) {
    line += ",\"level_raw\":";
    line += std::to_string(level);
  }
  if (level != gameLevel) {
    line += ",\"game_level\":";
    line += std::to_string(gameLevel);
  }
  line += ",\"msg\":\"";
  nevr_json_escape::AppendTo(line, message, len);
  line += "\"}\n";
}

}  // namespace nevr_run_log_record
