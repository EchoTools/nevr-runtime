#include "quest/cargo/map_mount.h"

#include <unistd.h>

#include <algorithm>
#include <utility>

#include "core/config_alias.h"
#include "quest/plugin/script_redirect.h"
#include "quest/sentinel/map_sets.h"

namespace nevr_quest::maps {

namespace {

namespace map_scripts = script_redirect::map_scripts;

constexpr const char kAliasPartial[] = "alias_partial";

nevr_maps::Grant Granted() {
  nevr_maps::Grant grant;
  grant.ok = true;
  return grant;
}

nevr_maps::Grant Refused(const char* reason) {
  nevr_maps::Grant grant;
  grant.reason = reason;
  return grant;
}

// The sets are disjoint (kCollides), so a mounted owner's set is the holder of a key some mounted set holds.
bool HoldsAlias(void* ctx, const std::string& owner, std::uint64_t alias) {
  const auto* mounted = static_cast<const std::set<std::string>*>(ctx);
  return mounted->count(owner) != 0 && map_sets::HoldsKey(nevr_config_alias::kAliasRecordTypeAndroid, alias);
}

}  // namespace

nevr_maps::Grant QuestMounter::Script(const std::string& owner, std::uint64_t hash, std::int64_t held,
                                      const std::string& sha256) {
  const char* refused = library_dir_.empty()
                            ? map_scripts::Register(owner.c_str(), hash, sha256.c_str())
                            : map_scripts::RegisterIn(library_dir_.c_str(), owner.c_str(), hash, sha256.c_str());
  if (refused != nullptr) return Refused(refused);
  ::close(static_cast<int>(held));
  return Granted();
}

nevr_maps::Grant QuestMounter::Rekey(const std::string&, std::uint64_t, std::uint64_t, const std::string&) {
  return Refused("script_unsupported");
}

nevr_maps::Grant QuestMounter::DerivedScript(const std::string&, std::uint64_t, std::int64_t, const std::string&,
                                             const std::string&) {
  return Refused("script_unsupported");
}

nevr_maps::Grant QuestMounter::Set(const std::string& owner, std::int64_t held, const std::string& label,
                                   const std::string& sha256, bool verified_stamp) {
  const nevr_resource_sets::Outcome outcome =
      map_sets::RegisterFileResourceSet(owner, held, label, sha256, !verified_stamp);
  if (!outcome.ok()) return Refused(nevr_resource_sets::RefusalName(outcome.refusal));
  return Granted();
}

nevr_maps::Grant QuestMounter::StageAliases(const std::string& owner, const std::string& id, std::uint64_t main_level,
                                            const std::vector<nevr_maps::ConfigAlias>& aliases) {
  if (aliases.empty()) return Granted();
  std::vector<nevr_config_alias::Pair> pairs;
  pairs.reserve(aliases.size());
  for (const nevr_maps::ConfigAlias& a : aliases) pairs.push_back({a.key, a.alias});
  return nevr_config_alias::Stage(owner, id, main_level, pairs) == pairs.size() ? Granted() : Refused(kAliasPartial);
}

void QuestMounter::Withdraw(const std::string& owner) {
  map_scripts::Withdraw(owner.c_str());
  nevr_config_alias::Unstage(owner);
  map_sets::Withdraw(owner);
}

bool HoldsMapScript(const std::string& owner, std::uint64_t hash) { return map_scripts::Holds(owner.c_str(), hash); }

MountOutcome ApplyMount(const std::vector<std::string>& expected, const nevr_resource_sets::MountReport& report) {
  MountOutcome out;
  std::set<std::string> mounted;
  nevr_config_alias::OwnerLevels owner_levels;
  if (report.sets != 0) {
    for (auto& [owner, levels] : map_sets::MountedOwnerLevels()) {
      if (std::find(expected.begin(), expected.end(), owner) == expected.end() || !mounted.insert(owner).second) continue;
      owner_levels.emplace_back(owner, std::move(levels));
    }
  }
  for (const std::string& owner : expected) {
    if (mounted.count(owner) != 0) {
      out.mounted.push_back(owner);
      continue;
    }
    map_scripts::Withdraw(owner.c_str());
    nevr_config_alias::Unstage(owner);
    out.withdrawn.push_back(owner);
  }
  out.armed = nevr_config_alias::Arm(owner_levels, &HoldsAlias, &mounted);
  return out;
}

}  // namespace nevr_quest::maps
