// The structured log of the store build's host tools (docs/standards/logging.md, tag NEVR.QUESTBUILD): one JSON object per
// line with `schema`, `ts`, `level`, `tag`, `event` and the step's own fields. A refusal is one line with the event `refused`,
// a `code` and a `detail`. Keys, passphrases and file contents are never fields.
#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace nevr_quest::obb {

class BuildLog {
 public:
  using Sink = std::function<void(const std::string& line)>;
  explicit BuildLog(Sink sink) : sink_(std::move(sink)) {}
  // Writes each line to standard output and flushes it.
  static BuildLog Stdout();

  void Info(const std::string& event, nlohmann::json fields = nlohmann::json::object());
  void Warning(const std::string& event, nlohmann::json fields = nlohmann::json::object());
  void Error(const std::string& event, nlohmann::json fields = nlohmann::json::object());

 private:
  void Emit(const char* level, const std::string& event, nlohmann::json fields);
  Sink sink_;
};

}  // namespace nevr_quest::obb
