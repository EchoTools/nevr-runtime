// The Quest plugin plan: which `plugins:` entries the loader will try, and which it refuses before reading a byte
// (docs/adr/0019-quest-plugin-loader.md, sections 2 and 3). Pure: std types only, no Android and no game headers, so the
// host test and the NDK build compile this one source.
//
// The entries are the PC's (PluginLoadItem, runtime/ext/plugin_load_plan.h) so one config file reads the same on both
// platforms. What differs on Quest:
//   * `file` is a basename in the native library directory. A path separator is `file_outside_directory`.
//   * The default `file` is `lib<name>.so`. The shared parser's default is `<name>.dll` (nevr_config.cpp), so a `file`
//     equal to that default is the Quest default. An entry that spells `<name>.dll` out gets the same treatment: no
//     PC DLL could load here anyway.
//   * `required` on a client logs and continues; nothing here is fatal.
#pragma once

#include <string>
#include <vector>

#include "runtime/ext/plugin_load_plan.h"

namespace nevr_quest::plugin {

// What became of one plan entry. Exactly one of these ends each entry (one `plugin_load` record).
enum class Outcome { kLoaded, kRefused, kDisabled, kNotStarted, kFailed };

const char* OutcomeName(Outcome outcome);
const char* PhaseName(PluginPhase phase);

// The stable reason tokens of a `plugin_load` record. A record's `reason` is one of these (or empty for `loaded`).
namespace reason {
inline constexpr const char* kDisabled = "disabled";
inline constexpr const char* kFileOutsideDirectory = "file_outside_directory";
inline constexpr const char* kDuplicateFile = "duplicate_file";
inline constexpr const char* kEarlyUnsupported = "early_unsupported";
inline constexpr const char* kMachineType = "machine_type";
inline constexpr const char* kDlopenFailed = "dlopen_failed";
inline constexpr const char* kNoPluginInfo = "no_plugin_info";
inline constexpr const char* kBuildIdsUndeclared = "build_ids_undeclared";
inline constexpr const char* kBuildIdUnsupported = "build_id_unsupported";
inline constexpr const char* kGameBuildIdUnavailable = "game_build_id_unavailable";
inline constexpr const char* kInitFailed = "init_failed";
inline constexpr const char* kException = "exception";
// not_started: the entry's install moment never came.
inline constexpr const char* kNoInstallMoment = "no_install_moment";
inline constexpr const char* kPostLoadHookUnavailable = "post_load_hook_unavailable";
inline constexpr const char* kGameExitedFirst = "game_exited_first";
}  // namespace reason

// `lib<name>.so`.
std::string DefaultFile(const std::string& name);

struct PlanEntry {
  PluginLoadItem item;          // item.file is the Quest file name (the default applied)
  PluginPhase phase = PluginPhase::Normal;
  bool pending = true;          // true until the entry has its one record
  Outcome outcome = Outcome::kNotStarted;
  std::string reasonToken;      // a reason:: token, empty for `loaded`
};

// One entry per item, in list order. An entry that is refused or disabled by the plan alone is not pending
// (outcome and reason set); every other entry is pending until the loader gives it its one record.
// `earlySupported` is false when a `dlopen` from the sentinel constructor is not known to work (ADR 0019 P10): an `early`
// entry is then refused as `early_unsupported`.
std::vector<PlanEntry> BuildPlan(std::vector<PluginLoadItem> items, bool earlySupported);

// The keys of an entry's args (never the values), sorted, joined with ','. `args_json` is the flat JSON object the shared
// plan builder wrote ("{}" when the entry has none).
std::string ArgKeys(const std::string& argsJson);

}  // namespace nevr_quest::plugin
