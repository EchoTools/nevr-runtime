#include "quest/plugin/plugin_record_log.h"

#include "quest/sentinel/hook_log.h"

namespace nevr_quest::plugin {

void LogLoadRecord(const LoadRecord& record) {
  const sentinel::LogField fields[] = {
      {"name", record.name.c_str()},
      {"file", record.file.c_str()},
      {"sha256", record.sha256.c_str()},
      {"expected_sha256", record.expectedSha256.c_str()},  // the manifest's hash; empty when the file has none
      {"plugin_build_id", record.pluginBuildId.c_str()},
      {"game_build_id", record.gameBuildId.c_str()},
      {"plugin_view_build_id", record.pluginViewBuildId.c_str()},
      {"libr15_patched", record.libr15Patched ? 1 : 0},
      {"phase", PhaseName(record.phase)},
      {"outcome", OutcomeName(record.outcome)},
      {"reason", record.reason.empty() ? "ok" : record.reason.c_str()},
      {"required", record.required ? 1 : 0},
      {"arg_keys", record.argKeys.c_str()},
      {"source", record.source.c_str()},
      {"overridden", record.overridden ? 1 : 0},
      {"config_rejected", record.configRejected ? 1 : 0},  // config.yaml was rejected whole: this plugin ran on its manifest
  };
  const bool fine = record.outcome == Outcome::kLoaded || record.outcome == Outcome::kDisabled;
  sentinel::LogFieldList(fine ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "plugin_load", fields,
                         sizeof(fields) / sizeof(fields[0]));
}

void LogPlanSummary(std::size_t entries, std::size_t early, std::size_t normal, std::size_t refused,
                    std::size_t disabled) {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "plugin_plan",
                      {{"entries", entries}, {"early", early}, {"normal", normal}, {"refused", refused},
                       {"disabled", disabled}});
}

}  // namespace nevr_quest::plugin
