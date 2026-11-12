/* A map's grants on the Quest (CDN_DESIGN §10.4 Q2, with Q3 and Q5): PC5a's Mounter over map_sets (Q1), the route-B map scripts
 * (script_redirect.h map_scripts) and ConfigAlias (core/config_alias.h), and the mount observer's half that withdraws and arms
 * them. The twin of PC5b's ChannelMounter and ApplyMount (runtime/content/map_channel_core.h), in PC5b's order: the scripts, then
 * the set, then the aliases at boot; at the mount, every owner the mount did not mount is withdrawn and the rest are armed.
 *
 * All or nothing per map: a map whose scripts, set or aliases are not all granted holds none of them. As on the PC, a map whose
 * aliases cannot all be staged is withdrawn whole by the core (nevr_maps::StageAliasesOrWithdraw).
 *
 * Kept apart from map_service.cpp so the host tests link it without the resource request hook.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/map_run.h"
#include "core/resource_set_registry.h"

namespace nevr_quest::maps {

class QuestMounter : public nevr_maps::Mounter {
 public:
  QuestMounter() = default;
  // Tests: map scripts are checked in `library_dir` instead of the sentinel's own library directory.
  explicit QuestMounter(std::string library_dir) : library_dir_(std::move(library_dir)) {}

  // Route B: the game opens the APK-packaged lib<hash16>.so itself, so the downloaded copy only carries the pin; on ok it is closed.
  nevr_maps::Grant Script(const std::string& owner, std::uint64_t hash, std::int64_t held, const std::string& sha256) override;
  // Refused `script_unsupported`: rekeys and rebuilt scripts need route A or recipes on the Quest.
  nevr_maps::Grant Rekey(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                         const std::string& stock_sha256) override;
  nevr_maps::Grant Set(const std::string& owner, std::int64_t held, const std::string& label, const std::string& sha256,
                       bool verified_stamp) override;
  // `alias_partial` when any pair was dropped; the caller then withdraws the map whole.
  nevr_maps::Grant StageAliases(const std::string& owner, const std::string& id, std::uint64_t main_level,
                                const std::vector<nevr_maps::ConfigAlias>& aliases) override;
  nevr_maps::Grant DerivedScript(const std::string& owner, std::uint64_t hash, std::int64_t held, const std::string& sha256,
                                 const std::string& stock_sha256) override;
  // The scripts, the aliases and the set.
  void Withdraw(const std::string& owner) override;

 private:
  std::string library_dir_;
};

// The registry's script waiver: a map set's script record passes while route B holds that script for that map.
bool HoldsMapScript(const std::string& owner, std::uint64_t hash);

struct MountOutcome {
  std::vector<std::string> mounted, withdrawn;  // owners
  std::size_t armed = 0;                        // alias pairs
};

// The mount observer's half, on the mounting thread after map_sets::Mount (or Abandon): each of `expected` (the owners the boot
// granted) that no mounted set belongs to has its scripts withdrawn and its aliases unstaged; the mounted ones are armed with
// their sets' levels, each pair while a mounted set holds its alias record.
MountOutcome ApplyMount(const std::vector<std::string>& expected, const nevr_resource_sets::MountReport& report);

}  // namespace nevr_quest::maps
