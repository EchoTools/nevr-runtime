#include "quest/obb/build_log.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace nevr_quest::obb {

namespace {
constexpr char kTag[] = "NEVR.QUESTBUILD";
constexpr int kSchema = 1;

std::string UtcNow() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  std::tm utc;
  gmtime_r(&seconds, &utc);
  char buffer[32];
  const std::size_t n = std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%S", &utc);
  std::string milliseconds = std::to_string(ms % 1000);
  milliseconds.insert(0, 3 - milliseconds.size(), '0');
  return std::string(buffer, n) + "." + milliseconds + "Z";
}
}  // namespace

BuildLog BuildLog::Stdout() {
  return BuildLog([](const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  });
}

void BuildLog::Info(const std::string& event, nlohmann::json fields) { Emit("info", event, std::move(fields)); }
void BuildLog::Warning(const std::string& event, nlohmann::json fields) { Emit("warning", event, std::move(fields)); }
void BuildLog::Error(const std::string& event, nlohmann::json fields) { Emit("error", event, std::move(fields)); }

void BuildLog::Emit(const char* level, const std::string& event, nlohmann::json fields) {
  nlohmann::json record = nlohmann::json::object();
  record["schema"] = kSchema;
  record["ts"] = UtcNow();
  record["level"] = level;
  record["tag"] = kTag;
  record["event"] = event;
  for (auto it = fields.begin(); it != fields.end(); ++it) record[it.key()] = it.value();
  sink_(record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

}  // namespace nevr_quest::obb
