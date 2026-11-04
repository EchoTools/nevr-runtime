#include "quest/obb/build_log.h"
namespace nevr_quest::obb {
BuildLog BuildLog::Stdout() { return BuildLog([](const std::string&) {}); }
void BuildLog::Info(const std::string&, nlohmann::json) {}
void BuildLog::Warning(const std::string&, nlohmann::json) {}
void BuildLog::Error(const std::string&, nlohmann::json) {}
void BuildLog::Emit(const char*, const std::string&, nlohmann::json) {}
}  // namespace nevr_quest::obb
