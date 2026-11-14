#include "quest/cargo/map_mount.h"

#include <unistd.h>

#include <algorithm>
#include <utility>

#include "core/config_alias.h"
#include "core/map_channel.h"
#include "quest/plugin/script_redirect.h"
#include "quest/sentinel/map_sets.h"

namespace nevr_quest::maps {

namespace {

namespace map_scripts = script_redirect::map_scripts;

constexpr const char kAliasPartial[] = "alias_partial";
constexpr const char kSignedOnly[] = "signed_only";
constexpr const char kScriptUnsupported[] = "script_unsupported";

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

}  // namespace

nevr_maps::Grant QuestMounter::Script(const std::string& owner, std::uint64_t hash, std::int64_t held,
                                      const std::string& sha256, bool official) {
  // ADR 0048 decision 7: under -require-signed-plugins only a manifest signed by a compiled-in official key counts.
  if (signed_only_ && !official) return Refused(kSignedOnly);
  const char* refused = library_dir_.empty()
                            ? map_scripts::Register(owner.c_str(), hash, sha256.c_str())
                            : map_scripts::RegisterIn(library_dir_.c_str(), owner.c_str(), hash, sha256.c_str());
  if (refused != nullptr) return Refused(refused);
  ::close(static_cast<int>(held));
  return Granted();
}

nevr_maps::Grant QuestMounter::Rekey(const std::string&, std::uint64_t, std::uint64_t, const std::string&) {
  return Refused(kScriptUnsupported);
}

nevr_maps::Grant QuestMounter::DerivedScript(const std::string&, std::uint64_t, std::int64_t, const std::string&,
                                             const std::string&, bool) {
  return Refused(kScriptUnsupported);
}

nevr_maps::Grant QuestMounter::Set(const std::string& owner, std::int64_t held, const std::string& label,
                                   const std::string& sha256, bool verified_stamp) {
  // The stamp the mount checked is the one the fetch recorded: no re-hash on the boot thread (ADR 0045 decision 7). A stamp nobody
  // verified asks for the re-hash, which only the worker passes.
  nevr_resource_sets::MapFileRequest request;
  request.owner = owner;
  request.label = label;
  request.sha256 = sha256;
  request.rehash = !verified_stamp;
  const nevr_resource_sets::Outcome outcome = map_sets::RegisterFileResourceSet(request, held);
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
  // The decision is the core's (core/map_channel.h DecideMount), the same on both platforms; the Quest supplies its owners' levels,
  // its withdraw and its alias record type.
  nevr_config_alias::OwnerLevels owner_levels;
  if (report.sets != 0) owner_levels = map_sets::MountedOwnerLevels();
  const nevr_map_channel::MountDecision decision = nevr_map_channel::DecideMount(
      expected, owner_levels,
      [](const std::string& owner) {
        map_scripts::Withdraw(owner.c_str());
        nevr_config_alias::Unstage(owner);
      },
      [](const std::string&, std::uint64_t alias) {
        return map_sets::HoldsKey(nevr_config_alias::kAliasRecordTypeAndroid, alias);
      });
  MountOutcome out;
  out.mounted = decision.mounted;
  out.withdrawn = decision.withdrawn;
  out.armed = decision.armed;
  return out;
}

}  // namespace nevr_quest::maps
