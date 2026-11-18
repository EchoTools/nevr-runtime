/* The custom-map channel's start (docs/adr/0048-custom-maps-channel.md; decisions 3, 7, 8, 12, 16 and 17 in part): the maps.* settings
 * read once for both platforms, the decision whether the channel runs, the boot mount of the cached groups, the background fetch's
 * plan and worker, and the seams the other parts of the channel plug into. The platforms supply the flat values (config.yaml), the
 * Env, MapFs, Mounter and Deriver, and the clock; nothing here touches the game, a thread or a directory until Boot decides to run.
 *
 * With no maps.base_url the channel does not run: Boot reports `not_configured` with one Info record, and makes no call to the Env
 * beyond that record. A configuration that cannot run (a key that does not parse, official and author keys together) is refused
 * with one Warning record and mounts nothing.
 *
 * Boot mounts the cached groups from the cache index with no network and no member hashing: a package index is held to its pin by
 * its stamp, and a moved stamp defers the map to the worker (ADR 0048 decision 4, ADR 0045 decision 7). The fetch runs on the worker
 * (FetchPass), never on the boot thread. A map fetched during the run mounts during the run (MountGroups).
 *
 * Derived from C-Luddy's custom-map channel (origin/cdn-construct-maps, src/runtime/content/map_channel.h and map_channel_local.cpp,
 * the FetchWorker class and the settings rules), reshaped onto the group-based core (core/map_run.h).
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cdn/cdn_env.h"
#include "core/config_alias.h"
#include "core/map_manifest.h"
#include "core/map_run.h"
#include "core/map_types.h"

namespace nevr_map_channel {

// The keys compiled into this build as official (ADR 0048 decision 12). Empty until the official key is chosen.
const std::vector<nevr_maps::PinnedKey>& CompiledOfficialKeys();

// The maps.* values by their flat keys (service_map.cpp: maps_base_url, maps_channel, maps_boot_budget_ms, maps_official_keys,
// maps_trusted_keys). A scalar answers "" when absent; a list answers one entry per item, in file order.
using ScalarLookup = std::function<std::string(const char* flat_key)>;
using ListLookup = std::function<std::vector<std::string>(const char* flat_key)>;

struct Settings {
  std::string reason;  // "" when the channel may run; else "not_configured", "bad_key" or "mixed_keys"
  std::string detail;  // names the refusal: the config key and the item's index, never a key's text
  std::string base_url;   // maps.base_url with a trailing '/'; empty when not configured
  std::string channel;    // maps.channel: the channel name the base URL is bound to ("" = the first verified manifest's)
  std::uint32_t boot_budget_ms = 0;  // maps.boot_budget_ms: how long one boot mount may take; 0 = no limit
  // The trust set: the compiled-in official keys (official and compiled), then maps.official_keys (official, never compiled), then
  // maps.trusted_keys (author keys, not official). Official and author keys are never both present (`mixed_keys`, decision 3).
  std::vector<nevr_maps::PinnedKey> keys;
};

// Reads the settings once. `compiled` is the build's official key list (CompiledOfficialKeys() in production). Nothing is parsed
// when maps.base_url is empty: the result is `not_configured`.
Settings ReadSettings(const ScalarLookup& value, const ListLookup& list, const std::vector<nevr_maps::PinnedKey>& compiled);

// Splits a comma-separated list into its trimmed, non-empty items (for a platform whose lookup answers comma-joined text).
std::vector<std::string> SplitCsv(const std::string& text);

enum class Start { kOff, kRefused, kRun };
// The start decision from the settings alone: kOff for `not_configured`, kRefused for any other reason, else kRun.
Start DecideStart(const Settings& settings);

// The groups the background fetch takes, in the order given (latest first): path-safe ids only, each once.
std::vector<std::string> FetchGroups(const std::vector<std::string>& recorded);

// Source ordering (ADR 0048 decision 8): a CDN map whose id a local copy holds is skipped, so the two never mix per key. The ids
// a local copy holds, sorted and unique: the channel passes them as ChannelConfig::local_maps.
std::vector<std::string> LocalShadowIds(const std::vector<std::string>& local_ids);

// The seams the platforms plug into the channel. An unset seam answers nothing.
//   recorded_groups: the lobby groups to fetch, latest first: the session pin's requests this run merged with the cache index's
//                    groups (RecordedGroupSeams; ADR 0048 decisions 15 and 16).
//   local_map_ids:   the ids a local map copy holds this run (local-copy precedence, ADR 0048 decision 8). Unset on both platforms:
//                    no reader lists hand-installed copies yet (the PC's installed_levels reads the game's own manifests, not copies).
struct Seams {
  std::function<std::vector<std::string>()> recorded_groups;
  std::function<std::vector<std::string>()> local_map_ids;
};
Seams NoSeams();

// The session pin's group requests (ADR 0048 decision 15) and the groups the cache holds (decision 16). ArmSessionGroupRequest sets
// nevr_maps::SetSessionGroupRequest's function to RecordRequestedGroup; a platform calls it once its channel runs, so until then a
// session success requests nothing. RecordRequestedGroup may be called from any thread: it takes a short lock, allocates nothing, and
// never throws (the Quest is built without exceptions). The queue keeps the latest kRequestedGroupSlots groups, each once.
void ArmSessionGroupRequest();
void RecordRequestedGroup(const std::string& group);
std::vector<std::string> RequestedGroups();  // the groups requested this run, latest first
// The requested groups in their order, then the cached groups none of them names (a group once, the first occurrence kept).
std::vector<std::string> MergeRecordedGroups(const std::vector<std::string>& requested, const std::vector<std::string>& cached);
// The recorded_groups seam for a platform: this run's requests merged with the cache index's groups in `dir`. Reads the index when
// called (the worker or the boot thread), never on the game's thread. `env` must outlive the seam.
Seams RecordedGroupSeams(nevr_cdn::Env& env, const std::string& dir);

// The mount observer's decision, the same on both platforms (ADR 0048 decisions 5 and 8): each expected owner that no mounted set
// belongs to is withdrawn (`withdraw`: its scripts and aliases; the set itself is the registry's); the mounted owners' aliases are
// armed with their levels, each pair while `holds_alias` says the owner's mounted set holds the alias record.
struct MountDecision {
  std::vector<std::string> mounted, withdrawn;
  std::size_t armed = 0;
};
MountDecision DecideMount(const std::vector<std::string>& expected, const nevr_config_alias::OwnerLevels& mounted_owners,
                          const std::function<void(const std::string& owner)>& withdraw,
                          const std::function<bool(const std::string& owner, std::uint64_t alias)>& holds_alias);

// A later mount's report folded into the run's (the run's report is what the session pin's view is taken from, ViewFromMount): what
// the later mount granted, refused or skipped is added, and a map's not-mounted token is the latest mount's. A map the later mount
// granted or found already mounted at its version is settled: it leaves the run's skipped, refused, deferred and not-mounted rows.
void MergeMountReport(nevr_maps::MountReport& into, const nevr_maps::MountReport& later);

// The ids a mount may grant (CacheMount::Mount's `only`): under ChannelConfig::mount_on_demand the ids a session asked for, else null,
// which grants every cached map the groups list. The one place the switch is read, on both platforms.
const std::vector<std::string>* MountScope(const nevr_maps::ChannelConfig& config, const std::vector<std::string>& asked);

struct BootResult {
  Start start = Start::kOff;
  std::string reason;  // "not_configured", the refusal, or "ok"
  nevr_maps::MountReport report;  // empty unless the channel ran
};

// One channel for one process. Boot is for the boot thread, once; FetchPass is for the worker.
class Channel {
 public:
  // `config` carries the platform's fields (dir, platform, game_build, stock hashes, is_server, signed_only); the base URL, channel,
  // boot budget and keys are taken from `settings`.
  Channel(nevr_maps::ChannelConfig config, const Settings& settings);
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  // The boot thread, once. A second call returns the first result and touches nothing: no Env call, no mount. `local_ids` are the
  // ids a local copy holds this run (LocalShadowIds applies here). The mount reads the cache index and mounts the cached groups
  // with no network and no member hashing.
  const BootResult& Boot(nevr_cdn::Env& env, nevr_maps::MapFs& fs, nevr_maps::Mounter& mounter, nevr_maps::Deriver* deriver,
                         const std::string& now_utc, const std::vector<std::string>& local_ids);

  bool booted() const { return booted_; }
  Start start() const { return start_; }
  const nevr_maps::ChannelConfig& config() const { return config_; }

  // The worker: fetches each group in order (FetchGroup, on demand: only the held maps are updated, see FetchScope), after Boot. A map
  // the boot mount deferred is hashed here, and only here. Stops early when keep_going turns false. Empty when the channel does not
  // run or Boot has not run.
  std::vector<nevr_maps::FetchReport> FetchPass(nevr_cdn::Env& env, nevr_maps::MapFs& fs, nevr_maps::Deriver* deriver,
                                                const std::vector<std::string>& groups, const std::string& now_utc,
                                                const std::function<bool()>& keep_going);

  // Lists the given groups' cached maps for the session pin without granting any of them: each map is reported (listed, and
  // not_mounted as not_asked) and none is mounted or registered, so a platform that mounts once per run (the Quest) can ask for a
  // map mid-run. Returns the run's report with every earlier listing folded in (FoldListing), the view the session pin reads.
  // Before Boot the report is empty. Worker thread only, after Boot.
  nevr_maps::MountReport ListGroups(const std::vector<std::string>& groups, const std::string& now_utc);

  // Mounts the given groups' cached maps now, in this process, through the boot's own CacheMount (one per process, so a map Boot
  // mounted at a version is not granted again): the hot mount of a map fetched during the run (ADR 0048 decision 16), and a server's
  // mount of the group it fetched (decision 13). `asked` are the ids a session asked for; they limit the mount under
  // ChannelConfig::mount_on_demand and are ignored without it (MountScope). The returned report is this call's; the run's report
  // (RunReport) is folded with it. The grants are registered with the platform's registry, which mounts them (the PC on its game
  // thread's next frame, the Quest on its mount thread): a mounted map is never replaced or evicted. Empty when Boot did not run the
  // channel. Worker thread only, after Boot.
  nevr_maps::MountReport MountGroups(const std::vector<std::string>& groups, const std::string& now_utc,
                                     const std::vector<std::string>& asked = std::vector<std::string>());

  // Every mount of the run folded into one report (the boot's, then each MountGroups): the session pin's view is taken from it.
  // Any thread; a copy taken under the channel's lock.
  nevr_maps::MountReport RunReport() const;

  // The maps this run has mounted (CacheMount::MountedIds): the in-use set every fetch of this channel passes as FetchScope::in_use, so
  // a fetch never evicts them. Empty before Boot mounts anything.
  std::vector<std::string> MountedIds() const;

 private:
  nevr_maps::ChannelConfig config_;
  Start start_ = Start::kOff;
  std::string refusal_, refusal_detail_;
  std::unique_ptr<nevr_maps::CacheMount> mount_;  // made by Boot once the channel runs; its references live as long as the channel
  bool booted_ = false;
  BootResult result_;
  mutable std::mutex run_mutex_;  // guards run_: the worker folds a mount in while the game's thread reads it
  // The boot's report, then each later mount folded in (MergeMountReport), and every ListGroups listing folded in (FoldListing).
  nevr_maps::MountReport run_;
};

// Folds one listing (ListGroups) into the run's report: every id the listing names replaces its earlier listed and not_mounted rows,
// the rows of other ids stay, and a map the run has mounted gets no not_mounted row (it is held, not waiting for a fetch).
void FoldListing(nevr_maps::MountReport* run, const nevr_maps::MountReport& listing);

// The gap between two extra passes a request may start (the worker's own limit on a request storm).
inline constexpr std::uint64_t kExtraFetchGapMs = 10ull * 60 * 1000;

// The background pass's thread: one pass, then it parks until a request (one more pass) or a stop. Heap-owned by the platform: on a
// stop that times out it is left alive with whatever its pass references. The pass must not throw (the build has no exceptions on
// the Quest). A request within the gap of the last extra pass is refused.
class FetchWorker {
 public:
  using Pass = std::function<void(const std::function<bool()>& keep_going)>;
  using Clock = std::function<std::uint64_t()>;
  // gap_ms: the gap between two extra passes (kExtraFetchGapMs on every platform; a test shortens it).
  FetchWorker(Pass pass, Clock now_ms, std::uint64_t gap_ms = kExtraFetchGapMs)
      : pass_(std::move(pass)), now_ms_(std::move(now_ms)), gap_ms_(gap_ms) {}
  FetchWorker(const FetchWorker&) = delete;
  FetchWorker& operator=(const FetchWorker&) = delete;

  void Start();
  // False when the last extra pass started under the gap ago, or the worker is stopping or not started. A refused request is not
  // dropped: the worker runs one pass for it when the gap ends, with no further request.
  bool Request();
  // False on a timeout: the thread is detached and this object must stay alive. A repeated or concurrent call only waits up to
  // `wait` for the worker to finish.
  bool Stop(std::chrono::milliseconds wait);
  std::size_t Passes() const { return passes_.load(); }
  bool started() const;

 private:
  void Run();
  void WaitForPass(std::unique_lock<std::mutex>& lock);

  Pass pass_;
  Clock now_ms_;
  std::uint64_t gap_ms_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false, requested_ = false, finished_ = false, started_ = false;
  bool pending_ = false;  // a request the gap refused; one pass runs it when the gap ends
  bool any_extra_ = false;
  std::uint64_t last_extra_ms_ = 0;
  std::atomic<bool> keep_going_{true};
  std::atomic<std::size_t> passes_{0};
  std::thread thread_;
};

}  // namespace nevr_map_channel
