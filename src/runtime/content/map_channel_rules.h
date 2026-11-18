// The decisions the PC map channel (content/map_channel.cpp) makes itself, apart from the core's (core/map_channel.h): which owners a
// mount's grants name, what the mount observer takes and hands the server's join hold, what a server's group fetch does with its
// outcome, how a dedicated server's worker loops, and when the boot refuses a run. Header-only with no Win32, game or channel state, so
// a host test drives each one with fakes (tests/test_map_channel_rules.cpp). map_channel.cpp calls them with the real seams.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config_alias.h"
#include "core/map_channel.h"
#include "core/map_fetch_queue.h"
#include "core/map_run.h"
#include "core/resource_set_registry.h"

namespace nevr_map_channel::pc {

/// The registry owners ("map:<id>") a mount granted: one per id in `report.mounted`, in order. Maps kept from an earlier mount, deferred,
/// skipped or refused are not granted, so no withdrawal check is owed for them.
inline std::vector<std::string> GrantedOwners(const nevr_maps::MountReport& report) {
  std::vector<std::string> owners;
  owners.reserve(report.mounted.size());
  for (const std::string& id : report.mounted) owners.push_back(std::string(nevr_resource_sets::kMapOwnerPrefix) + id);
  return owners;
}

/// The mounted map owners and their levels, from the registry's report (a JSON array of sets): the entries whose "plugin" is "map:<id>",
/// each with the hex level hashes of its "levels" list. Anything else in the report, and a report that is not an array, is left out.
inline nevr_config_alias::OwnerLevels MountedOwners(const std::string& report_json) {
  nevr_config_alias::OwnerLevels owners;
  const nlohmann::json doc = nlohmann::json::parse(report_json, nullptr, false);
  if (!doc.is_array()) return owners;
  for (const nlohmann::json& set : doc) {
    if (!set.is_object()) continue;
    const auto plugin = set.find("plugin");
    if (plugin == set.end() || !plugin->is_string()) continue;
    const std::string owner = plugin->get<std::string>();
    if (owner.rfind(nevr_resource_sets::kMapOwnerPrefix, 0) != 0) continue;
    std::vector<std::uint64_t> levels;
    const auto listed = set.find("levels");
    if (listed != set.end() && listed->is_array()) {
      for (const nlohmann::json& level : *listed) {
        if (level.is_string()) levels.push_back(std::strtoull(level.get<std::string>().c_str(), nullptr, 16));
      }
    }
    owners.emplace_back(owner, std::move(levels));
  }
  return owners;
}

/// What the worker has granted that the mount observer has not decided yet, and the server's fetched groups whose join hold waits for the
/// mount that takes them. Any thread (the observer runs on the mounting thread, the grants on the worker).
class GrantLedger {
 public:
  struct Taken {
    std::vector<std::string> granted;                                    // owners granted since the last Take
    std::vector<std::pair<std::string, nevr_maps::MountReport>> holds;  // groups whose hold is released once this mount ran
  };

  void Grant(const nevr_maps::MountReport& report) {
    std::vector<std::string> owners = GrantedOwners(report);
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::string& owner : owners) granted_.push_back(std::move(owner));
  }

  void Hold(const std::string& group, const nevr_maps::MountReport& report) {
    std::lock_guard<std::mutex> lock(mutex_);
    holds_.emplace_back(group, report);
  }

  /// Hands over everything and empties the ledger: each mount decides only the grants made since the one before it.
  Taken Take() {
    Taken taken;
    std::lock_guard<std::mutex> lock(mutex_);
    taken.granted.swap(granted_);
    taken.holds.swap(holds_);
    return taken;
  }

 private:
  std::mutex mutex_;
  std::vector<std::string> granted_;
  std::vector<std::pair<std::string, nevr_maps::MountReport>> holds_;
};

/// What a dedicated server's fetch of one group does with its outcome. A failed fetch tells the join hold (`note_failed`) and mounts
/// nothing: a group never mounted holds to its timeout, a mounted one goes ahead on what is mounted. Any other outcome mounts the group
/// (`mount`), records it for the hold's release (the ledger), then registers its grants (`install`). Returns `report` unchanged.
template <class NoteFailed, class Mount, class Install>
nevr_maps::FetchReport FinishServerGroup(const std::string& group, const nevr_maps::FetchReport& report, GrantLedger& ledger,
                                         NoteFailed&& note_failed, Mount&& mount, Install&& install) {
  if (report.outcome == nevr_maps::FetchOutcome::kFailed) {
    note_failed(group);
    return report;
  }
  const nevr_maps::MountReport mounted = mount(group);
  ledger.Hold(group, mounted);
  install(mounted);
  return report;
}

/// A dedicated server's worker: while `keep_going`, waits up to `wait_ms` for a requested group and runs the oldest through `fetch`.
template <class KeepGoing, class Fetch>
void RunServerPass(nevr_maps::GroupFetchQueue& queue, KeepGoing&& keep_going, std::uint32_t wait_ms, Fetch&& fetch) {
  while (keep_going()) {
    if (!queue.WaitForRequest(wait_ms)) continue;
    queue.RunNext(fetch);
  }
}

enum class BootRefusal { kNone, kUnsupportedGameBuild, kNoRoot };

/// Why the boot refuses to run the channel. Only a run the core decided to start (Start::kRun) can be refused: the game build the map
/// hooks are for is checked first (`supported_build` is called only then), then the nEVR root.
template <class SupportedBuild>
BootRefusal PreflightRefusal(Start start, SupportedBuild&& supported_build, bool root_empty) {
  if (start != Start::kRun) return BootRefusal::kNone;
  if (!supported_build()) return BootRefusal::kUnsupportedGameBuild;
  if (root_empty) return BootRefusal::kNoRoot;
  return BootRefusal::kNone;
}

}  // namespace nevr_map_channel::pc
