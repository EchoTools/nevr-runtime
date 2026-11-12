/* The custom-map channel's core (docs/adr/0047; CDN_DESIGN §4.2, §4.6, §6, §10.2 PC5a): the boot mount from the cache
 * and the background fetch that fills it. Pure over nevr_cargo::Env, MapFs, a Mounter and a Deriver, so the same code
 * runs in the runtime and against fakes.
 *
 * BootMount runs on the boot thread: no network and no hashing beyond the stored manifest and each package.json.
 * FetchAll runs on the background worker and never mounts: what it stores mounts at the next launch. Neither runs on
 * the game thread.
 *
 * Every outcome is a structured event under the env's prefix ([NEVR.MAPS]): map_boot, map_mounted, map_skipped,
 * map_deferred, map_summary at boot; map_gc, map_rehash, map_manifest, map_plan, map_index, map_piece, map_member,
 * map_derive, map_ready, map_done in the fetch.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/cargo_env.h"
#include "core/map_manifest.h"
#include "core/map_types.h"

namespace nevr_maps {

struct Grant {
  bool ok = false;
  std::string reason;  // a registry or provider token
};

// Implemented by PC5b over PC3, PC6 and PC8. On ok the callee owns `held`; on a refusal the caller keeps it.
class Mounter {
 public:
  virtual ~Mounter() = default;
  virtual Grant Script(const std::string& owner, std::uint64_t hash, std::int64_t held, const std::string& sha256) = 0;
  virtual Grant Rekey(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                      const std::string& stock_sha256) = 0;
  virtual Grant Set(const std::string& owner, std::int64_t held, const std::string& label, const std::string& sha256,
                    bool verified_stamp) = 0;
  // !ok when any pair was dropped (not allowed, or over 4); reason "alias_partial". The callee only stages: the
  // decision to withdraw the map is StageAliasesOrWithdraw's, on both platforms.
  virtual Grant StageAliases(const std::string& owner, const std::string& id, std::uint64_t main_level,
                             const std::vector<ConfigAlias>& aliases) = 0;
  // A rekeyed script PC10 rebuilt (non-empty edits).
  virtual Grant DerivedScript(const std::string& owner, std::uint64_t hash, std::int64_t held,
                              const std::string& sha256, const std::string& stock_sha256) = 0;
  virtual void Withdraw(const std::string& owner) = 0;  // undo every grant of `owner`
};

// §13 D8 on both platforms: a map whose aliases are not all staged keeps nothing it registered (set, scripts, rebuilt
// scripts, aliases), or it would mount on stock rules. "" when every pair was staged, else the refusal token.
inline std::string StageAliasesOrWithdraw(Mounter& mounter, const std::string& owner, const std::string& id,
                                          std::uint64_t main_level, const std::vector<ConfigAlias>& aliases) {
  if (aliases.empty()) return std::string();
  const Grant grant = mounter.StageAliases(owner, id, main_level, aliases);
  if (grant.ok) return std::string();
  mounter.Withdraw(owner);
  return grant.reason.empty() ? "alias_partial" : grant.reason;
}

struct ChannelConfig {
  std::string dir, base_url, platform = kPlatformPc, game_build;
  std::vector<std::string> subscribed, stock_manifest_sha256;
  std::vector<std::string> server_allow;  // "<id>" or "<id>@<index sha256>"
  std::vector<std::string> local_maps;    // ids EchoMapLoader will load in this run
  std::vector<PinnedKey> keys;
  bool is_server = false, signed_only = false;
  std::uint64_t cap_bytes = 8ull << 30;
  std::string channel;               // maps.channel; empty = the channel base_url was bound to at its first fetch
  std::uint32_t boot_budget_ms = 0;  // 0 = no limit
};

struct MountedMap {
  std::string id, version, index_sha256, platform, source_sha256, title;
  std::uint64_t level = 0;
};

struct BootReport {
  std::vector<std::string> mounted, deferred, skipped;
  std::vector<std::pair<std::string, std::string>> refused;   // (id, token)
  std::vector<MountedMap> staged;                             // the `mounted` maps in detail (PC9)
  std::vector<std::pair<std::uint64_t, std::string>> listed;  // rad_hash(id) -> id, every map listed for this platform
  std::vector<std::pair<std::string, std::string>> not_mounted;  // (id, token) for skipped, deferred and refused
};

BootReport BootMount(nevr_cargo::Env& env, MapFs& fs, const ChannelConfig& config, Mounter& mounter, Deriver* deriver,
                     const std::string& now_utc);

struct FetchReport {
  std::string reason;  // "" when the pass ran to its end
  std::size_t maps_updated = 0, pieces_fetched = 0, pieces_reused = 0;
  std::uint64_t bytes_fetched = 0;
};

// `deferred` is BootReport::deferred: those maps' files are hashed again before the plan.
FetchReport FetchAll(nevr_cargo::Env& env, MapFs& fs, const ChannelConfig& config,
                     const std::vector<std::string>& deferred, Deriver* deriver, const std::string& now_utc,
                     const std::function<bool()>& keep_going);

// Exposed for the test: the frame walk FetchAll records as frames_checked. True when the file is a resource set whose
// tables lie inside it and every chunk's frame starts with the zstd magic and its frame header states the chunk's
// decompressed size.
bool WalkSetFrames(nevr_cargo::Env& env, const std::string& path, std::string* problem);

}  // namespace nevr_maps
