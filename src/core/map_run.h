/* The custom-map channel's core (docs/adr/0048-custom-maps-channel.md): the fetch that fills the cache with one lobby group's maps,
 * and the mount that hands the cached maps of a list of groups to the registry. Pure over nevr_cdn::Env, MapFs, a Mounter and a
 * Deriver, so the same code runs on the PC, on the Quest and against fakes.
 *
 * Groups (ADR 0048): each lobby group has one signed manifest, <base_url>/guilds/<group id>/manifest, whose body names the group;
 * a manifest that names another group is refused (guild_mismatch). The group id is opaque (core/map_manifest.h,
 * IsPathSafeGroupId). A client and a server both take their group from the lobby session, and no config key chooses a group. The
 * cache index keeps the groups fetched, the latest first, as many as the device's storage share holds (ADR 0045 decision 8); the
 * maps it keeps are the union of what those groups' manifests list. Objects are content addressed: a map two groups list at one
 * version, and a piece two maps share, are fetched and stored once.
 *
 * FetchGroup runs on the worker (core/map_fetch_queue.h queues the requests), never on the game's frame or the boot thread. It
 * fetches the group's manifest, plans the maps it lists, and stores each one: every piece is hashed once, as it arrives, and
 * appended to the member file in a staging directory (Env::OpenAppend; a failed piece's bytes are cut off again, Truncate), which
 * is synced and then renamed into place (MapFs::RenameDir). A piece some cached file already holds is copied from that file when
 * the file's stamp still equals the one recorded when it was verified, and is not hashed again. A stopped or failed transfer
 * keeps the pieces it verified: the staging directory records them, and the next fetch resumes after them. The cache index is
 * written last: it is the commit point.
 *
 * CacheMount::Mount reads the cache and grants what it holds: no network, and no hashing beyond the stored manifests' signatures
 * (ADR 0048 decision 3). A package index is held to its pin by the stamp the cache index recorded when the fetch hashed it, and a
 * moved stamp defers the map to the worker (decision 4), so the mount may run on the boot thread (ADR 0045 decision 7). It can
 * be called again, after a fetch, to mount what arrived.
 *
 * Every outcome is a structured event (the env's tag is [NEVR.MAPS]): map_limits, map_mount_group, map_mounted, map_skipped,
 * map_deferred, map_summary in the mount; map_limits, map_index, map_gc, map_rehash, map_manifest, map_trust_changed, map_plan,
 * map_piece, map_member, map_derive, map_ready, map_done in the fetch.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cdn/cdn_env.h"
#include "core/map_local_precedence.h"
#include "core/map_manifest.h"
#include "core/map_types.h"

namespace nevr_maps {

struct Grant {
  bool ok = false;
  std::string reason;  // a registry or provider token
};

// The platform's grants: the resource-set registry, the script provider and the config aliases. On ok the callee owns `held`; on
// a refusal the caller keeps it.
class Mounter {
 public:
  virtual ~Mounter() = default;
  // `official`: the manifest that pins the script verified under a key compiled into this build (ADR 0048 decision 7). A provider
  // under -require-signed-plugins refuses a script whose `official` is false.
  virtual Grant Script(const std::string& owner, std::uint64_t hash, std::int64_t held, const std::string& sha256,
                       bool official) = 0;
  virtual Grant Rekey(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                      const std::string& stock_sha256) = 0;
  virtual Grant Set(const std::string& owner, std::int64_t held, const std::string& label, const std::string& sha256,
                    bool verified_stamp) = 0;
  // !ok when any pair was dropped (not allowed, or over the limit); reason "alias_partial". The callee only stages: the decision
  // to withdraw the map is StageAliasesOrWithdraw's, on both platforms.
  virtual Grant StageAliases(const std::string& owner, const std::string& id, std::uint64_t main_level,
                             const std::vector<ConfigAlias>& aliases) = 0;
  // A rekeyed script the recipe rebuild made (non-empty edits).
  virtual Grant DerivedScript(const std::string& owner, std::uint64_t hash, std::int64_t held,
                              const std::string& sha256, const std::string& stock_sha256, bool official) = 0;
  virtual void Withdraw(const std::string& owner) = 0;  // undo every grant of `owner`
};

// ADR 0048 decision 5, on both platforms: a map whose aliases are not all staged keeps nothing it registered (set, scripts,
// rebuilt scripts, aliases), or it would mount on stock rules. "" when every pair was staged, else the refusal token.
inline std::string StageAliasesOrWithdraw(Mounter& mounter, const std::string& owner, const std::string& id,
                                          std::uint64_t main_level, const std::vector<ConfigAlias>& aliases) {
  if (aliases.empty()) return std::string();
  const Grant grant = mounter.StageAliases(owner, id, main_level, aliases);
  if (grant.ok) return std::string();
  mounter.Withdraw(owner);
  return grant.reason.empty() ? "alias_partial" : grant.reason;
}

// maps.cache_cap_gib's default: the most the map cache holds on disk.
inline constexpr std::uint64_t kDefaultCacheCapBytes = 20ull << 30;

// What the channel is told by config.yaml (the `maps` section) and by the platform. Nothing here chooses a group.
struct ChannelConfig {
  std::string dir, base_url, platform = kPlatformPc, game_build;
  std::vector<std::string> stock_manifest_sha256;
  std::vector<std::string> local_maps;  // ids a local map loader loads in this run: local names (ADR 0048 decision 8)
  // The provider seam (core/map_local_precedence.h): the level names of this run's local copies. Empty by default, which is no
  // local level; the channel supplies the platform's.
  LocalLevelNamesFn local_level_names;
  std::vector<PinnedKey> keys;
  bool signed_only = false;          // -require-signed-plugins: a map's scripts count only under an official key (ADR 0048 decision 7)
  std::string channel;               // maps.channel; empty = the channel base_url was bound to at its first fetch
  std::uint32_t boot_budget_ms = 0;  // maps.boot_budget_ms: how long one mount may take; 0 = no limit
  std::uint64_t cache_cap_bytes = kDefaultCacheCapBytes;  // maps.cache_cap_gib: the most the cache holds; 0 = no cap
  // The platform's: a map mounts only when a session asks for it, after its group's manifest was checked, so a version the
  // bucket moved past since the last run is never mounted at boot. False mounts every cached map at boot (the Quest, which has
  // no later mount yet).
  bool mount_on_demand = false;
  // False: a map a session's request fetches mid-run is stored, not mounted, and plays after the next start; the request still
  // lists its group's maps, so the session pin names what it lacks. The Quest: its map sets mount once per run
  // (quest/sentinel/map_sets.h Mount), so a set registered later would be refused too late.
  bool mounts_mid_run = true;
};

// Where a group's manifest is on the channel, and where the cache keeps the copy it verified.
std::string GroupManifestUrl(const std::string& base_url, const std::string& group);
std::string StoredGroupManifestPath(const std::string& dir, const std::string& group);

// The groups the cache index keeps, the latest first: what a client mounts at launch and fetches again in the background.
std::vector<std::string> CachedGroupIds(nevr_cdn::Env& env, const std::string& dir);

// The files a mount holds open take at most 1 part in this many of the descriptors the process has free (ADR 0045 decision 8);
// the texture cargo's store files take their own share (cdn/cdn_limits.h, kDescriptorShareForStore).
inline constexpr std::uint64_t kDescriptorShareForMaps = 4;

// The refusal token of a cached version that no verified manifest lists now (`refused` of map_mounted, a Warning): the map does not
// mount until the listed version is stored, so an old version never plays in its place.
inline constexpr const char* kStaleToken = "stale";
// The skip token of a map an earlier mount of this process holds at another version: a mounted set is never replaced in the process.
inline constexpr const char* kMountedOtherVersionToken = "mounted_other_version";
// The skip token of a cached, current map no session has asked for yet (ChannelConfig::mount_on_demand).
inline constexpr const char* kNotAskedToken = "not_asked";

struct MountedMap {
  std::string id, version, index_sha256, platform, source_sha256, title, group;
  std::uint64_t level = 0;
};

struct MountReport {
  std::vector<std::string> mounted, deferred, skipped;
  std::vector<std::string> kept;  // mounted by an earlier call at the same version: not granted again
  std::vector<std::pair<std::string, std::string>> refused;         // (id, token)
  std::vector<MountedMap> staged;                                    // the `mounted` maps in detail
  std::vector<std::pair<std::uint64_t, std::string>> listed;        // rad_hash(id) -> id, every map listed for this platform
  std::vector<std::pair<std::string, std::string>> not_mounted;     // (id, token) for skipped, deferred and refused
  std::vector<std::pair<std::string, std::string>> groups_refused;  // (group, token): its stored manifest did not verify
};

// Mounts the maps the given groups' stored manifests list, from the cache. `groups` is the latest first: a map two groups list is
// mounted at the version the first of them lists. Each call reads the cache again, so a call after a fetch mounts what arrived; a
// map an earlier call mounted at the same version is reported in `kept` and not granted again, and one an earlier call mounted at
// another version is skipped (mounted_other_version): a mounted map is not replaced in this process.
class CacheMount {
 public:
  CacheMount(nevr_cdn::Env& env, MapFs& fs, const ChannelConfig& config, Mounter& mounter, Deriver* deriver);
  // `only`, when given, are the ids this call may grant; every other cached map at its listed version is skipped (not_asked).
  MountReport Mount(const std::vector<std::string>& groups, const std::string& now_utc,
                    const std::vector<std::string>* only = nullptr);
  std::vector<std::string> MountedIds() const;  // every map a call mounted, sorted

 private:
  nevr_cdn::Env& env_;
  MapFs& fs_;
  const ChannelConfig& config_;
  Mounter& mounter_;
  Deriver* deriver_;
  std::map<std::string, std::string> mounted_;           // id -> the index sha256 it was mounted at
  std::map<std::uint64_t, std::string> script_holders_;  // script or rekey hash -> the id that mounted it
};

enum class FetchOutcome {
  kFetched,  // the group's manifest verified and at least one map was stored
  kCached,   // the group's manifest verified and the cache already held every map it lists that this run can use
  kFailed,   // the manifest could not be had or verified, or a map it lists could not be stored: `reason` says why
};
const char* FetchOutcomeName(FetchOutcome outcome) noexcept;

// How far a group's fetch has come, for a caller that bounds a wait by the download rate. `set_bytes` is what the group's
// manifest lists for this platform and `bytes_to_fetch` the part of it the cache does not hold, both known from the manifest
// before any piece is requested; `bytes_fetched` grows with every piece, and `elapsed_ms` is measured from the fetch's start on
// the env's clock.
struct FetchProgress {
  std::string group;
  std::uint64_t set_bytes = 0;
  std::uint64_t bytes_to_fetch = 0;
  std::uint64_t bytes_fetched = 0;
  std::uint64_t elapsed_ms = 0;
};
using FetchProgressFn = std::function<void(const FetchProgress&)>;

struct FetchReport {
  std::string group;
  FetchOutcome outcome = FetchOutcome::kFailed;
  std::string reason;  // "" unless the outcome is kFailed
  std::size_t maps_updated = 0, pieces_fetched = 0, pieces_reused = 0;
  std::uint64_t set_bytes = 0, bytes_to_fetch = 0, bytes_fetched = 0, elapsed_ms = 0;
  // The first map the cache had no room for (reason over_budget): what it needs, the most the budget could make free for it, and
  // the cap (0 = none).
  std::string no_room_id;
  std::uint64_t no_room_needed = 0, no_room_available = 0, no_room_cap = 0;
};

// Which of the maps a group's manifest lists one fetch stores. By default every listed map (a server's group, ADR 0048 decision 13).
// On demand (a client's fetch): only the ids the caller names (the map its session needs) and the maps the cache already holds at a
// version the manifest no longer lists, so a bucket of many maps costs a player only the maps it plays. A named id the manifest
// does not list for this platform fails the fetch (not_listed); nothing outside the verified manifest is ever fetched.
//
// The cache budget: what the cache holds stays within ChannelConfig::cache_cap_bytes, and what one fetch adds within the device's
// storage share. A map that does not fit evicts whole maps, the least recently played first (never played counts as oldest), and
// fails over_budget when evicting every map it may evict still leaves too little. Never evicted: `in_use` (the maps this run
// mounted, stamped as played now), the ids the scope names, the maps this fetch stores, and on a whole-group fetch (a server's
// held group) every map the group lists.
struct FetchScope {
  bool on_demand = false;
  std::vector<std::string> ids;
  std::vector<std::string> in_use;
};

// Fetches one group's manifest and the maps `scope` takes of it. `deferred` are maps a mount deferred (MountReport::deferred):
// their files are hashed again before the plan. `progress` (optional) is called after the plan and after every piece, on the
// worker.
FetchReport FetchGroup(nevr_cdn::Env& env, MapFs& fs, const ChannelConfig& config, const std::string& group,
                       const std::vector<std::string>& deferred, Deriver* deriver, const std::string& now_utc,
                       const std::function<bool()>& keep_going, const FetchProgressFn& progress = nullptr,
                       const FetchScope& scope = FetchScope());

}  // namespace nevr_maps
