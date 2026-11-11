// The sentinel log, captured for a combat host test: install it with Install(), read the lines of one event with Lines(). A line is the
// JSON object the sentinel writes, so a test compares the text from "event" on (Golden) and looks for a field with Has.
#pragma once

#include <string>
#include <vector>

#include "quest/sentinel/hook_log.h"

namespace combat_test {

struct Line {
  sentinel::LogLevel level;
  std::string text;
};

inline std::vector<Line>& Captured() {
  static std::vector<Line> lines;
  return lines;
}
inline void Capture(sentinel::LogLevel level, const char* line) { Captured().push_back({level, line}); }
inline void StartCapture() {
  Captured().clear();
  sentinel::SetLogSink(&Capture);
}
inline void StopCapture() { sentinel::SetLogSink(nullptr); }

inline bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// The captured lines of one event, in order.
inline std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : Captured()) {
    if (Has(l.text, key)) out.push_back(l);
  }
  return out;
}

// The line from its "event" key on: the part a test compares whole (the leading timestamp and level differ from run to run).
inline std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}

}  // namespace combat_test
