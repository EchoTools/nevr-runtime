// The map channel's fetch and mount (core/map_run.h; docs/adr/0048-custom-maps-channel.md) over the shared fake Env and its MapFs
// (quest/tests/cdn_fake_env.h, map_fake_fs.h), a scripted server, a recording Mounter and Deriver, and the real parsers fed
// group manifests this test signs with fixed-seed keys. No network, no Win32, no game file. Log assertions read each record's
// fields.
#include "core/map_run.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/base64.h"
#include "cdn/cdn_limits.h"
#include "core/map_channel.h"
#include "core/map_fetch_queue.h"
#include "core/map_request_serve.h"
#include "core/map_channel.h"
#include "core/map_index.h"
#include "core/map_request.h"
#include "core/map_set_tables.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "nevr/resource_set.h"
#include "nevr/symbol_hash.h"
#include "quest/tests/cdn_fake_env.h"
#include "quest/tests/map_fake_fs.h"

#include "quest/tests/map_run_fixture.h"

namespace {

using namespace map_run_fixture;

// The cache budget's sizes (see the budget tests below): kSmallMap is the chunks of the small map, kLargeMap is bigger than one small map
// and no bigger than two (one eviction is not enough for it), and kTinyFreeBytes is a free space whose storage share is one byte, so
// nothing fits and eviction decides.
constexpr int kSmallMap = 2;
constexpr int kLargeMap = 4;
constexpr std::uint64_t kTinyFreeBytes = 1;
constexpr const char* kPlayedFirst = "2026-11-11T00:00:01Z";
constexpr const char* kPlayedSecond = "2026-11-11T00:00:02Z";

// The GTest fixture: the shared fixture (map_run_fixture::Fixture), with the assertions a test makes on its own reads.
class MapRunTest : public ::testing::Test, public Fixture {
 protected:
  // The index a test asserts on: its read's status is checked here, so a failed read fails the test.
  CacheIndex Index() {
    CacheIndex index;
    EXPECT_EQ(ReadCacheIndex(env, kDir, &index), CacheIndexStatus::kOk);
    return index;
  }
  // Every handle the mount opened is the mounter's, or closed.
  void ExpectNoLeak() { EXPECT_EQ(env.open_blobs(), mounter.Owned()); }
  // Whether the cache index holds the map (its current or its last version).
  bool Cached(const std::string& id) { return Index().maps.count(MapKey(id, kPlatformPc)) != 0; }
  // A client's fetch of the ids it asks for, at `now`, with `in_use` the maps this run mounted (FetchScope, map_run.h).
  FetchReport FetchOnDemand(const std::vector<std::string>& ids, const std::string& now, const std::vector<std::string>& in_use = {}) {
    FetchScope scope;
    scope.on_demand = true;
    scope.ids = ids;
    scope.in_use = in_use;
    return FetchGroup(env, fs, config, kGroup, {}, nullptr, now, keep_going, nullptr, scope);
  }
  // `mapped` (a configured channel over this fixture's cache) is booted over the empty cache, then the first group is fetched and
  // mounted, as a server does. Then the second group is published alone (the first group's map is unlisted by it) and the device has
  // room for nothing, so the second group's map fits only if the first group's mounted map is evicted.
  void MountFirstGroupThenListOnlyASecondMap(nevr_map_channel::Channel& mapped) {
    ASSERT_EQ(mapped.Boot(env, fs, mounter, nullptr, kNow, {}).reason, "ok");
    channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
    channel.Sign(1);
    Fetch();
    const MountReport mounted = mapped.MountGroups({kGroup}, kNow);
    ASSERT_EQ(mounted.mounted, (std::vector<std::string>{"alpha"}));
    channel.group = kOtherGroup;
    channel.Unpublish("alpha");
    channel.Publish("gamma", "1.0.0", MakeSet(kSmallMap, 'c'));
    channel.Sign(2);
    env.device.free_bytes = kTinyFreeBytes;
  }
};

// The cache budget (ADR 0045 decision 8; docs/reference/maps-channel.md). What one fetch may add is the device's storage share of the
// free space, read when the fetch starts. An on-demand fetch that needs more room evicts the least recently played maps it may evict
// (never played is oldest), never a map this run mounted; a map with no room is refused, and nothing is evicted for it.
namespace {
// The free space at which the device's storage share (free less 1 part in kStorageShareKept) holds `bytes`.
std::uint64_t FreeFor(std::uint64_t bytes) {
  std::uint64_t free_bytes = bytes;
  while (free_bytes - free_bytes / nevr_cdn::kStorageShareKept < bytes) ++free_bytes;
  return free_bytes;
}
}  // namespace

// The budget follows the device's free space: the same fetch that fits without eviction on a roomy device evicts on a small one.
TEST_F(MapRunTest, TheBudgetIsTheDevicesFreeSpaceShare) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(kSmallMap, 'b'));
  channel.Sign(1);
  Fetch();  // the whole group: alpha and beta cached, never played
  channel.Publish("gamma", "1.0.0", MakeSet(kSmallMap, 'c'));
  channel.Sign(2);
  env.device.free_bytes = FreeFor(MakeSet(kSmallMap, 'c').size());  // a storage share that holds exactly one more small map
  const FetchReport fetched = FetchOnDemand({"gamma"}, kNow);
  EXPECT_EQ(fetched.maps_updated, 1u);
  EXPECT_EQ(env.FindRecord("map_gc", "reason", "budget"), nullptr);
  EXPECT_TRUE(Cached("alpha"));
  EXPECT_TRUE(Cached("beta"));
  EXPECT_EQ(Index().maps.at(MapKey("gamma", kPlatformPc)).last_played_utc, kNow);  // a map the session asks for is played now
}

TEST_F(MapRunTest, TheSameFetchEvictsTheNeverPlayedMapWhenTheDeviceHasLessRoom) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(kSmallMap, 'b'));
  channel.Sign(1);
  Fetch();
  FetchOnDemand({"beta"}, kPlayedFirst);  // beta is played; alpha never is
  channel.Publish("gamma", "1.0.0", MakeSet(kSmallMap, 'c'));
  channel.Sign(2);
  env.device.free_bytes = kTinyFreeBytes;
  const FetchReport fetched = FetchOnDemand({"gamma"}, kNow);
  EXPECT_EQ(fetched.maps_updated, 1u);
  EXPECT_NE(env.FindRecord("map_gc", "evicted", MapKey("alpha", kPlatformPc)), nullptr);
  EXPECT_FALSE(Cached("alpha"));
  EXPECT_TRUE(Cached("beta"));
  EXPECT_TRUE(Cached("gamma"));
}

// Two played maps: the one played longer ago goes first, and one eviction is enough, so the later one stays. A play time never moves
// back: replaying alpha at an earlier time keeps its later stamp, so beta, played first, is still the one evicted.
TEST_F(MapRunTest, EvictsTheLeastRecentlyPlayedFirst) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(kSmallMap, 'b'));
  channel.Sign(1);
  Fetch();
  FetchOnDemand({"beta"}, kPlayedFirst);
  FetchOnDemand({"alpha"}, kPlayedSecond);
  FetchOnDemand({"alpha"}, kPlayedFirst);
  EXPECT_EQ(Index().maps.at(MapKey("alpha", kPlatformPc)).last_played_utc, kPlayedSecond);
  channel.Publish("gamma", "1.0.0", MakeSet(kSmallMap, 'c'));
  channel.Sign(2);
  env.device.free_bytes = kTinyFreeBytes;
  const FetchReport fetched = FetchOnDemand({"gamma"}, kNow);
  EXPECT_EQ(fetched.maps_updated, 1u);
  EXPECT_NE(env.FindRecord("map_gc", "evicted", MapKey("beta", kPlatformPc)), nullptr);
  EXPECT_EQ(env.FindRecord("map_gc", "evicted", MapKey("alpha", kPlatformPc)), nullptr);
  EXPECT_FALSE(Cached("beta"));
  EXPECT_FALSE(fs.HasDir(channel.VersionDir("beta")));  // the evicted version's directory is removed
  EXPECT_TRUE(Cached("alpha"));
}

// Maps never played are oldest, and ties go by id: alpha goes before beta.
TEST_F(MapRunTest, NeverPlayedMapsGoByIdWhenTheyTie) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(kSmallMap, 'b'));
  channel.Sign(1);
  Fetch();
  channel.Publish("gamma", "1.0.0", MakeSet(kSmallMap, 'c'));
  channel.Sign(2);
  env.device.free_bytes = kTinyFreeBytes;
  const FetchReport fetched = FetchOnDemand({"gamma"}, kNow);
  EXPECT_EQ(fetched.maps_updated, 1u);
  EXPECT_NE(env.FindRecord("map_gc", "evicted", MapKey("alpha", kPlatformPc)), nullptr);
  EXPECT_TRUE(Cached("beta"));
}

// A map the session asks for is kept though it is played and the oldest candidate would be another. The large map needs both small
// ones gone, so with alpha kept the fetch is refused and beta, the only victim left, stays too.
TEST_F(MapRunTest, AMapTheSessionAsksForIsNeverEvicted) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(kSmallMap, 'b'));
  channel.Sign(1);
  Fetch();
  FetchOnDemand({"alpha"}, kPlayedSecond);  // alpha is played; beta never is
  channel.Publish("gamma", "1.0.0", MakeSet(kLargeMap, 'c'));
  channel.Sign(2);
  env.device.free_bytes = kTinyFreeBytes;
  const FetchReport fetched = FetchOnDemand({"alpha", "gamma"}, kNow);
  EXPECT_EQ(fetched.reason, "over_budget");
  EXPECT_EQ(env.FindRecord("map_gc", "reason", "budget"), nullptr);
  EXPECT_TRUE(Cached("alpha"));
  EXPECT_TRUE(Cached("beta"));
}

// The budget is one pass's: the first map takes the room and the second, which the room does not hold, is refused.
TEST_F(MapRunTest, TwoNewMapsShareOneBudget) {
  channel.Publish("gamma", "1.0.0", MakeSet(kSmallMap, 'c'));
  channel.Publish("delta", "1.0.0", MakeSet(kSmallMap, 'd'));
  channel.Sign(1);
  env.device.free_bytes = FreeFor(MakeSet(kSmallMap, 'c').size());  // a storage share that holds exactly one small map
  const FetchReport fetched = FetchOnDemand({"gamma", "delta"}, kNow);
  EXPECT_EQ(fetched.maps_updated, 1u);
  EXPECT_EQ(fetched.reason, "over_budget");
  EXPECT_TRUE(Cached("delta"));  // the manifest is planned in id order, so delta takes the room first
  EXPECT_FALSE(Cached("gamma"));
}

// Each new map takes the next victim, not the one before it: with no room, gamma evicts alpha and delta must then evict beta.
TEST_F(MapRunTest, EachNewMapTakesTheNextVictim) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(kSmallMap, 'b'));
  channel.Sign(1);
  Fetch();
  FetchOnDemand({"alpha"}, kPlayedFirst);
  FetchOnDemand({"beta"}, kPlayedSecond);
  channel.Publish("gamma", "1.0.0", MakeSet(kSmallMap, 'c'));
  channel.Publish("delta", "1.0.0", MakeSet(kSmallMap, 'd'));
  channel.Sign(2);
  env.device.free_bytes = kTinyFreeBytes;
  const FetchReport fetched = FetchOnDemand({"gamma", "delta"}, kNow);
  EXPECT_EQ(fetched.maps_updated, 2u);
  EXPECT_FALSE(Cached("alpha"));
  EXPECT_FALSE(Cached("beta"));  // the second map's victim, not a second count of alpha's bytes
  EXPECT_TRUE(Cached("gamma"));
  EXPECT_TRUE(Cached("delta"));
}

// A new version of a map keeps the play time of the version it replaces.
TEST_F(MapRunTest, AnUpdatedMapKeepsItsPlayTime) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Sign(1);
  Fetch();
  FetchOnDemand({"alpha"}, kPlayedFirst);
  channel.Publish("alpha", "1.1.0", MakeSet(kSmallMap, 'b'));
  channel.Sign(2);
  Fetch();  // the whole group: alpha's update is stored, and no id asks for it
  EXPECT_EQ(Index().maps.at(MapKey("alpha", kPlatformPc)).version, "1.1.0");
  EXPECT_EQ(Index().maps.at(MapKey("alpha", kPlatformPc)).last_played_utc, kPlayedFirst);
}

// A map this run mounted keeps its superseded version's directory: the update is stored, and the GC of the next pass leaves the old
// version in place while this run mounts the map; the next run's GC removes it.
TEST_F(MapRunTest, AMountedMapKeepsItsSupersededVersionUntilTheNextRun) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Sign(1);
  Fetch();
  const std::string old_version = channel.VersionDir("alpha");
  channel.Publish("alpha", "1.1.0", MakeSet(kSmallMap, 'b'));
  channel.Sign(2);
  const std::vector<std::string> in_use = {"alpha"};
  FetchOnDemand({}, kNow, in_use);  // stores 1.1.0; the old version is still the one the index named at this pass's GC
  FetchOnDemand({}, kNow, in_use);  // this pass's GC finds 1.0.0 unreferenced
  EXPECT_EQ(Index().maps.at(MapKey("alpha", kPlatformPc)).version, "1.1.0");
  EXPECT_TRUE(fs.HasDir(old_version));
}

// A map this run mounted keeps its directory and its index entry when the group stops listing it; the next run's GC removes it.
TEST_F(MapRunTest, AMountedMapIsKeptByTheGcWhileThisRunMountsIt) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Sign(1);
  Fetch();
  channel.Unpublish("alpha");
  channel.Sign(2);
  const std::vector<std::string> in_use = {"alpha"};
  FetchOnDemand({}, kNow, in_use);  // the group loses alpha at the end of this pass
  FetchOnDemand({}, kNow, in_use);  // and this pass's GC would remove it, were it not mounted
  EXPECT_TRUE(fs.HasDir(std::string(kDir) + "/alpha"));
  EXPECT_TRUE(Cached("alpha"));
  EXPECT_EQ(env.FindRecord("map_gc", "reason", "unlisted"), nullptr);
}

// The maps this run mounted are never evicted, and a refused map evicts nothing: the two mounted maps are the only ones that could
// make room for the large map, so the fetch is refused and both stay, mounted, with their directories.
TEST_F(MapRunTest, AMapThisRunMountedIsNeverEvicted) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(kSmallMap, 'b'));
  channel.Sign(1);
  Fetch();
  CacheMount mount(env, fs, config, mounter, nullptr);
  mount.Mount({kGroup}, kNow);
  const std::vector<std::string> in_use = mount.MountedIds();
  EXPECT_EQ(in_use, (std::vector<std::string>{"alpha", "beta"}));
  channel.Publish("gamma", "1.0.0", MakeSet(kLargeMap, 'c'));
  channel.Sign(2);
  env.device.free_bytes = kTinyFreeBytes;
  const FetchReport fetched = FetchOnDemand({"gamma"}, kPlayedSecond, in_use);
  EXPECT_EQ(fetched.reason, "over_budget");
  EXPECT_EQ(env.FindRecord("map_gc", "reason", "budget"), nullptr);
  EXPECT_TRUE(Cached("alpha"));
  EXPECT_TRUE(Cached("beta"));
  EXPECT_TRUE(fs.HasDir(std::string(kDir) + "/alpha"));
  EXPECT_TRUE(fs.HasDir(std::string(kDir) + "/beta"));
  EXPECT_EQ(Index().maps.at(MapKey("alpha", kPlatformPc)).last_played_utc, kPlayedSecond);
}

// The channel's fetch keeps the maps it mounted this run: Channel::FetchPass passes FetchScope::in_use, built from Channel::MountedIds.
// A client's pass is on demand (ADR 0048 decision 16): with nothing asked it stores no map, so gamma is left alone (not_needed) and
// alpha stays. The second group does not list alpha, so only the in_use scope keeps it when a session asks for gamma: without it the
// garbage collection drops alpha as unlisted, and gamma is refused over_budget with alpha gone. Here alpha stays, with its directory and
// its index entry, in both cases.
TEST_F(MapRunTest, AFetchPassKeepsTheMapsTheChannelMountedThisRun) {
  nevr_map_channel::Settings settings;
  settings.base_url = kBase;
  settings.keys = {key.pin};
  nevr_map_channel::Channel mapped(config, settings);
  MountFirstGroupThenListOnlyASecondMap(mapped);
  const std::vector<FetchReport> passes = mapped.FetchPass(env, fs, nullptr, {kOtherGroup}, kPlayedSecond, keep_going);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_NE(env.FindRecord("map_plan", "reason", "not_needed"), nullptr);
  EXPECT_EQ(env.FindRecord("map_plan", "reason", "over_budget"), nullptr);
  EXPECT_FALSE(Cached("gamma"));
  EXPECT_TRUE(Cached("alpha"));
  EXPECT_TRUE(fs.HasDir(std::string(kDir) + "/alpha"));

  FetchScope scope;
  scope.on_demand = true;
  scope.ids = {"gamma"};
  scope.in_use = mapped.MountedIds();
  const FetchReport asked = FetchGroup(env, fs, config, kOtherGroup, {}, nullptr, kPlayedSecond, keep_going, nullptr, scope);
  EXPECT_EQ(asked.maps_updated, 0u);
  EXPECT_NE(env.FindRecord("map_plan", "reason", "over_budget"), nullptr);
  EXPECT_EQ(env.FindRecord("map_gc", "reason", "unlisted"), nullptr);
  EXPECT_TRUE(Cached("alpha"));
  EXPECT_TRUE(fs.HasDir(std::string(kDir) + "/alpha"));
  EXPECT_FALSE(Cached("gamma"));
}

// A dedicated server fetches each group with the same in_use scope (runtime/content/map_channel.cpp, FetchServerGroup, from
// Channel::MountedIds): the second group's fetch keeps the first group's mounted map, the same as the channel's own pass.
TEST_F(MapRunTest, ASecondServerGroupDoesNotEvictTheFirstGroupsMountedMap) {
  nevr_map_channel::Settings settings;
  settings.base_url = kBase;
  settings.keys = {key.pin};
  nevr_map_channel::Channel mapped(config, settings);
  MountFirstGroupThenListOnlyASecondMap(mapped);
  FetchScope scope;
  scope.in_use = mapped.MountedIds();
  ASSERT_EQ(scope.in_use, (std::vector<std::string>{"alpha"}));
  const FetchReport fetched = FetchGroup(env, fs, config, kOtherGroup, {}, nullptr, kPlayedSecond, keep_going, nullptr, scope);
  EXPECT_EQ(fetched.maps_updated, 0u);
  EXPECT_NE(env.FindRecord("map_plan", "reason", "over_budget"), nullptr);
  EXPECT_EQ(env.FindRecord("map_gc", "reason", "unlisted"), nullptr);
  EXPECT_TRUE(Cached("alpha"));
  EXPECT_TRUE(fs.HasDir(std::string(kDir) + "/alpha"));
}

// A refusal says why, in the record: the bytes the map needs and the room the device gives once every map it may evict is gone.
TEST_F(MapRunTest, ANoRoomRefusalNamesWhyAndEvictsNothing) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Sign(1);
  Fetch();
  channel.Publish("gamma", "1.0.0", MakeSet(kLargeMap, 'c'));
  channel.Sign(2);
  const std::uint64_t needed = MakeSet(kLargeMap, 'c').size();
  const std::uint64_t small = MakeSet(kSmallMap, 'a').size();
  env.device.free_bytes = kTinyFreeBytes;
  const FetchReport fetched = FetchOnDemand({"gamma"}, kNow);
  EXPECT_EQ(fetched.reason, "over_budget");
  const Record* plan = env.FindRecord("map_plan", "reason", "over_budget");
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(plan->level, nevr_cdn::EventLevel::kWarn);
  EXPECT_EQ(plan->Get("needed"), std::to_string(needed));
  EXPECT_EQ(plan->Get("available"), std::to_string(kTinyFreeBytes + small));
  EXPECT_FALSE(plan->Get("consequence").empty());
  EXPECT_EQ(env.FindRecord("map_gc", "reason", "budget"), nullptr);
  EXPECT_TRUE(Cached("alpha"));
}

// A whole-group fetch keeps every map the group lists, so a fetch over the budget evicts none of them.
TEST_F(MapRunTest, AWholeGroupFetchEvictsNothingItLists) {
  channel.Publish("alpha", "1.0.0", MakeSet(kSmallMap, 'a'));
  channel.Sign(1);
  Fetch();
  channel.Publish("gamma", "1.0.0", MakeSet(kSmallMap, 'c'));
  channel.Sign(2);
  env.device.free_bytes = kTinyFreeBytes;
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.reason, "over_budget");
  EXPECT_TRUE(Cached("alpha"));
  EXPECT_EQ(env.FindRecord("map_gc", "reason", "budget"), nullptr);
}

TEST_F(MapRunTest, AnOfflineMountMountsFromTheCacheWithoutNetworkOrMemberHashing) {
  const std::string set_bytes = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", set_bytes);
  channel.Sign(1);
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.reason, "");
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFetched);
  EXPECT_EQ(fetched.maps_updated, 1u);
  EXPECT_EQ(fetched.pieces_fetched, 7u);

  env.server.clear();
  const std::size_t gets = env.requests.size();
  env.hashed_bytes = 0;
  const MountReport boot = Boot();
  EXPECT_EQ(env.requests.size(), gets);
  EXPECT_LT(env.hashed_bytes, set_bytes.size());
  ASSERT_TRUE(Mounted(boot, "alpha"));
  EXPECT_TRUE(mounter.verified["map:alpha"]);
  EXPECT_EQ(mounter.labels["map:alpha"], "map:alpha@1.0.0");
  EXPECT_EQ(mounter.set_sha["map:alpha"], Sha(set_bytes));
  ExpectNoLeak();
  const Record* summary = env.FindRecord("map_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->level, nevr_cdn::EventLevel::kInfo);
  EXPECT_EQ(summary->Get("mounted"), "1");
}

// Each byte is hashed once, as it arrives: the fetch hashes the pieces and the package index, and never a stored file.
TEST_F(MapRunTest, TheFetchHashesEachByteOnceWhileItArrives) {
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(6, 'a'));
  const std::string index = env.server[std::string(kBase) + "blobs/sha256/" + channel.IndexSha("alpha")].body;
  channel.Sign(1);
  env.hashed_bytes = 0;
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  EXPECT_EQ(env.hashed_bytes, p.members[0].size + index.size());
}

TEST_F(MapRunTest, AScriptRefusedSecondOfThreeWithdrawsAndRegistersNoSet) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll one"}, {2, "dll two"}, {3, "dll three"}});
  channel.Sign(1);
  Fetch();
  mounter.refuse_scripts = {2};
  const MountReport boot = Boot();
  EXPECT_FALSE(Mounted(boot, "alpha"));
  EXPECT_EQ(mounter.calls, (std::vector<std::string>{"script map:alpha 1", "script map:alpha 2", "withdraw map:alpha"}));
  EXPECT_EQ(TokenOf(boot, "alpha"), "script:pe_rules");
  ExpectNoLeak();
  EXPECT_EQ(env.open_blobs(), 0u);
}

TEST_F(MapRunTest, ASetRefusedAtItsGrantWithdrawsTheScripts) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll one"}});
  channel.Sign(1);
  Fetch();
  mounter.refuse_sets = {"map:alpha"};
  const MountReport boot = Boot();
  EXPECT_FALSE(Mounted(boot, "alpha"));
  EXPECT_TRUE(mounter.Called("withdraw map:alpha"));
  EXPECT_EQ(TokenOf(boot, "alpha"), "collides");
  EXPECT_EQ(env.open_blobs(), 0u);
}

TEST_F(MapRunTest, TwoMapsSharingAScriptHashTheLowerIdWins) {
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'), {{9, "shared dll"}});
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{9, "shared dll"}});
  channel.Sign(1);
  Fetch();
  const MountReport boot = Boot();
  EXPECT_TRUE(Mounted(boot, "alpha"));
  EXPECT_FALSE(Mounted(boot, "beta"));
  EXPECT_EQ(TokenOf(boot, "beta"), "script:duplicate");
  const Record* line = env.FindRecord("map_mounted", "id", "beta");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->Get("holder"), "map:alpha");
  EXPECT_FALSE(mounter.Called("script map:beta 9"));
  ExpectNoLeak();
}

TEST_F(MapRunTest, ALocalMapIsSkippedWithNoGrant) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Sign(1);
  Fetch();
  config.local_maps = {"alpha"};
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "local_map");
  EXPECT_TRUE(mounter.calls.empty());
  EXPECT_EQ(env.open_blobs(), 0u);
}

TEST_F(MapRunTest, ACrashBeforeTheIndexMountsNoOldVersionAndTheNextGcRemovesTheNewDirectory) {
  channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Sign(1);
  Fetch();
  const std::string v1_dir = channel.VersionDir("alpha");
  channel.Publish("alpha", "1.1.0", MakeSet(4, 'b'));
  channel.Sign(2);
  const std::string v2_dir = channel.VersionDir("alpha");
  env.fail_writes = {std::string(kDir) + "/index.json"};
  const FetchReport crashed = Fetch();
  EXPECT_EQ(crashed.reason, "index_write");
  EXPECT_EQ(crashed.outcome, FetchOutcome::kFailed);
  EXPECT_TRUE(fs.HasDir(v2_dir));

  // The stored manifest lists 1.1.0 and the cache holds 1.0.0 only: the old version is refused stale, never mounted in its place.
  const MountReport boot = Boot();
  EXPECT_TRUE(boot.staged.empty());
  EXPECT_EQ(TokenOf(boot, "alpha"), kStaleToken);

  env.fail_writes.clear();
  env.server.clear();  // offline: the GC runs, the fetch then fails at the manifest
  Fetch();
  EXPECT_FALSE(fs.HasDir(v2_dir));
  EXPECT_TRUE(fs.HasDir(v1_dir));
}

// ADR 0048 decision 7: under -require-signed-plugins a map's scripts count only when an official key signed its manifest; a
// script-free map still mounts.
TEST_F(MapRunTest, UnderSignedOnlyOnlyAnOfficialManifestsScriptsMount) {
  channel.Publish("plain", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("scripted", "1.0.0", MakeSet(2, 'b'), {{1, "dll"}});
  channel.Sign(1);
  Fetch();
  config.signed_only = true;
  MountReport boot = Boot();
  EXPECT_TRUE(Mounted(boot, "plain"));
  EXPECT_EQ(TokenOf(boot, "scripted"), "signed_only");

  TestKey official{1, true, true};
  config.keys = {official.pin};
  mounter.calls.clear();
  boot = Boot();
  EXPECT_TRUE(Mounted(boot, "scripted"));
  ASSERT_FALSE(mounter.script_official.empty());
  EXPECT_TRUE(mounter.script_official.front());
}

// ADR 0048 decision 7: a key set in maps.official_keys is official but not compiled in, so it never satisfies the flag, and its
// manifest's scripts are refused signed_only.
TEST_F(MapRunTest, AConfiguredOfficialKeyDoesNotSatisfySignedOnly) {
  channel.Publish("scripted", "1.0.0", MakeSet(2, 'b'), {{1, "dll"}});
  channel.Sign(1);
  Fetch();
  config.signed_only = true;
  TestKey configured{1, true, false};
  config.keys = {configured.pin};
  EXPECT_EQ(TokenOf(Boot(), "scripted"), "signed_only");
  EXPECT_TRUE(mounter.script_official.empty());
}

// A cached version no verified manifest lists any longer (its newer version is not stored yet) is refused stale, with or without
// -require-signed-plugins: the old version never mounts in place of the listed one (ADR 0048 decision 3).
TEST_F(MapRunTest, ACachedVersionNoManifestListsIsRefusedStale) {
  TestKey official{1, true};
  config.keys = {official.pin};
  channel.Publish("scripted", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Sign(1);
  Fetch();
  channel.Publish("scripted", "1.1.0", MakeSet(2, 'b'), {{1, "dll two"}});
  channel.Sign(2);
  env.fail_writes = {std::string(kDir) + "/index.json"};  // the new manifest is stored, the new version is not
  Fetch();
  config.signed_only = true;
  EXPECT_EQ(TokenOf(Boot(), "scripted"), kStaleToken);
  config.signed_only = false;
  const MountReport boot = Boot();
  EXPECT_TRUE(boot.staged.empty());
  EXPECT_EQ(TokenOf(boot, "scripted"), kStaleToken);
  EXPECT_FALSE(mounter.Called("set map:scripted"));
}

TEST_F(MapRunTest, TheSummaryListsEveryRefusalAndRefusedGroup) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  channel.Publish("gamma", "1.0.0", MakeSet(2, 'c'), {}, [](Package& p) { AddAlias(p); });
  channel.Sign(1);
  Fetch();
  mounter.refuse_scripts = {1};
  mounter.refuse_sets = {"map:beta"};
  mounter.alias["map:gamma"] = Grant{false, "alias_partial"};
  const MountReport boot = Boot(nullptr, kNow, {kGroup, kOtherGroup});
  EXPECT_FALSE(Mounted(boot, "gamma"));
  const Record* summary = env.FindRecord("map_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->level, nevr_cdn::EventLevel::kWarn);
  EXPECT_EQ(summary->Get("mounted"), "0");
  EXPECT_EQ(summary->Get("refused"), "alpha:script:pe_rules,beta:collides,gamma:alias_partial");
  EXPECT_EQ(summary->Get("groups_refused"), std::string(kOtherGroup) + ":no_manifest");
}

// Decision 5, both platforms: a map with two aliases, one of them refused, keeps nothing it registered (its set and its script
// are withdrawn and their files closed), is refused once as alias_partial, and frees its script hash, so a later map with the
// same script still mounts. The mounter only stages; the core withdraws.
TEST_F(MapRunTest, AMapWithARefusedAliasIsWithdrawnWholeAndFreesItsScripts) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{9, "shared dll"}}, [](Package& p) {
    AddAlias(p);
    ConfigAlias refused;
    refused.key_name = "mp_combat_rules";
    refused.alias_name = p.id + "_combat_rules";
    refused.key = EchoVR::CSymbol64Hash(refused.key_name.c_str());
    refused.alias = EchoVR::CSymbol64Hash(refused.alias_name.c_str());
    p.config_aliases.push_back(refused);
  });
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'), {{9, "shared dll"}});
  channel.Sign(1);
  Fetch();
  mounter.alias["map:alpha"] = Grant{false, ""};
  const MountReport boot = Boot();
  EXPECT_FALSE(Mounted(boot, "alpha"));
  EXPECT_EQ(TokenOf(boot, "alpha"), "alias_partial");
  EXPECT_TRUE(Mounted(boot, "beta"));
  EXPECT_TRUE(mounter.Called("set map:alpha"));
  EXPECT_TRUE(mounter.Called("withdraw map:alpha"));
  EXPECT_EQ(mounter.Owned("map:alpha"), 0u);
  EXPECT_EQ(std::count_if(boot.not_mounted.begin(), boot.not_mounted.end(),
                          [](const auto& r) { return r.first == "alpha" && r.second == "alias_partial"; }),
            1);
  EXPECT_EQ(std::count_if(boot.refused.begin(), boot.refused.end(), [](const auto& r) { return r.first == "alpha"; }), 1);
  ASSERT_EQ(boot.staged.size(), 1u);
  EXPECT_EQ(boot.staged[0].id, "beta");
  const Record* summary = env.FindRecord("map_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->Get("mounted"), "1");
  EXPECT_EQ(summary->Get("refused"), "alpha:alias_partial");
}

TEST_F(MapRunTest, ANullDeriverRefusesARecipeMapAndARecipeFreeMapStillMounts) {
  channel.Publish("plain", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("recipe", "1.0.0", MakeSet(2, 'b'), {}, [](Package& p) { AddRecipe(p); });
  channel.Sign(1);
  Fetch();
  EXPECT_NE(env.FindRecord("map_derive", "reason", "derived_unsupported"), nullptr);
  const MountReport boot = Boot();
  EXPECT_TRUE(Mounted(boot, "plain"));
  EXPECT_EQ(TokenOf(boot, "recipe"), "not_cached");
}

TEST_F(MapRunTest, ARecipeMapMountsItsMergedSetAndItsRebuiltScript) {
  constexpr std::uint64_t kRebuiltHash = 0x77, kStockScript = 0x55;
  constexpr std::size_t kScriptBytes = 128, kEditAt = 16;
  deriver.rebuilt_scripts[kRebuiltHash] = std::string(kScriptBytes, 'R');
  channel.Publish("recipe", "1.0.0", MakeSet(3, 'r'), {}, [&](Package& p) {
    AddRecipe(p);
    RekeyPin pin;
    pin.hash = kRebuiltHash;
    pin.stock_source = kStockScript;
    pin.stock_sha256 = Sha("stock dll");
    pin.edits.push_back(FieldEdit{kEditAt, sizeof(std::uint64_t), p.level});
    pin.result_sha256 = Sha(std::string(kScriptBytes, 'R'));
    pin.result_size = kScriptBytes;
    p.rekeyed.push_back(pin);
  });
  channel.Sign(1);
  EXPECT_EQ(Fetch(&deriver).maps_updated, 1u);
  EXPECT_EQ(deriver.derive_calls, 1);
  const CacheIndex index = Index();
  ASSERT_EQ(index.maps.count("recipe/windows-x86_64"), 1u);
  EXPECT_EQ(index.maps.at("recipe/windows-x86_64").stock_stamps.size(), 1u);  // one per distinct path

  const MountReport boot = Boot(&deriver);
  ASSERT_TRUE(Mounted(boot, "recipe"));
  EXPECT_TRUE(mounter.Called("derived map:recipe 119"));
  EXPECT_TRUE(mounter.verified["map:recipe"]);
  EXPECT_EQ(deriver.stock_checked.size(), 1u);
  ExpectNoLeak();
}

TEST_F(MapRunTest, StockCurrentRunsOncePerDistinctStockFileAcrossMaps) {
  channel.Publish("one", "1.0.0", MakeSet(2, 'a'), {}, [](Package& p) { AddRecipe(p); });
  channel.Publish("two", "1.0.0", MakeSet(2, 'b'), {}, [](Package& p) { AddRecipe(p); });
  channel.Sign(1);
  Fetch(&deriver);
  const MountReport boot = Boot(&deriver);
  EXPECT_EQ(boot.mounted.size(), 2u);
  EXPECT_EQ(deriver.stock_checked, (std::vector<std::string>{"C:/game/_data/packages/stock0"}));
}

TEST_F(MapRunTest, AStockChangeDefersTheMapAndOneFetchRebuildsItForTheNextMount) {
  channel.Publish("recipe", "1.0.0", MakeSet(2, 'r'), {}, [](Package& p) { AddRecipe(p); });
  channel.Sign(1);
  Fetch(&deriver);
  deriver.current = false;
  const MountReport deferred = Boot(&deriver);
  EXPECT_EQ(TokenOf(deferred, "recipe"), "stock_changed");
  EXPECT_EQ(deferred.deferred, (std::vector<std::string>{"recipe"}));

  const std::size_t blob_gets = GetsUnder(std::string(kBase) + "blobs/");
  Fetch(&deriver, deferred.deferred);  // the index sha is unchanged; the fetch must rebuild anyway
  EXPECT_EQ(deriver.derive_calls, 2);
  EXPECT_EQ(GetsUnder(std::string(kBase) + "blobs/"), blob_gets);
  EXPECT_NE(env.FindRecord("map_rehash", "result", "rebuilt"), nullptr);

  deriver.current = true;
  mounter.calls.clear();
  EXPECT_TRUE(Mounted(Boot(&deriver), "recipe"));
}

TEST_F(MapRunTest, ABadFrameRecordsFramesCheckedFalseAndTheSetNeverTakesTheVerifiedPath) {
  std::string set_bytes = MakeSet(3, 'a');
  set_bytes[FramesStart(3) + kFrameBytes] = 0;  // frame 1 loses its zstd magic
  channel.Publish("alpha", "1.0.0", set_bytes);
  channel.Sign(1);
  Fetch();
  EXPECT_FALSE(Index().maps.at("alpha/windows-x86_64").files[0].frames_checked);
  const Record* line = env.FindRecord("map_member", "frames", "frame_magic");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->Get("index"), "1");
  EXPECT_TRUE(Mounted(Boot(), "alpha"));  // the registry's own frame check still runs at the mount
  EXPECT_FALSE(mounter.verified["map:alpha"]);
}

// The set file's tables and every frame header, judged by the cargo validator (core/map_set_tables.h).
TEST_F(MapRunTest, TheSetCheckJudgesEveryFrameHeader) {
  const std::uint64_t table_bytes = std::uint64_t{1} << 20;
  fs.Put("C:/t/good", MakeSet(5, 'x'));
  EXPECT_EQ(CheckSetFile(env, "C:/t/good", table_bytes).reason, "");

  std::string size = MakeSet(5, 'x');
  size[FramesStart(5) + 2 * kFrameBytes + 5] = 99;  // frame 2 states 99 bytes, the chunk 100
  fs.Put("C:/t/size", size);
  SetProblem problem = CheckSetFile(env, "C:/t/size", table_bytes);
  EXPECT_EQ(problem.reason, "content_size_mismatch");
  EXPECT_EQ(problem.index, 2u);

  std::string range = MakeSet(2, 'x');
  range.resize(range.size() - 1);  // the last frame runs past the end
  fs.Put("C:/t/range", range);
  problem = CheckSetFile(env, "C:/t/range", table_bytes);
  EXPECT_EQ(problem.reason, "chunk_range");
  EXPECT_EQ(problem.index, 1u);

  fs.Put("C:/t/short", "NEVRRES1");
  EXPECT_EQ(CheckSetFile(env, "C:/t/short", table_bytes).reason, "too_small");
  // Tables the memory share cannot hold are not read.
  EXPECT_EQ(CheckSetFile(env, "C:/t/good", SetTableMemoryBytes(5, 5) - 1).reason, "no_memory");
  EXPECT_EQ(env.open_blobs(), 0u);
}

TEST_F(MapRunTest, TheBootBudgetSkipsTheLaterIds) {
  for (const char* id : {"alpha", "beta", "gamma"}) channel.Publish(id, "1.0.0", MakeSet(2, id[0]));
  channel.Sign(1);
  Fetch();
  config.boot_budget_ms = 100;
  mounter.set_cost_ms = 60;
  const MountReport boot = Boot();
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"alpha", "beta"}));
  EXPECT_EQ(TokenOf(boot, "gamma"), "boot_budget");
  EXPECT_NE(env.FindRecord("map_summary", "skipped", "gamma:boot_budget"), nullptr);
}

TEST_F(MapRunTest, StagedListedAndNotMountedAreFilled) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  channel.Publish("other", "1.0.0", MakeSet(2, 'o'));
  channel.Sign(1);
  Fetch();
  config.local_maps = {"beta"};
  const MountReport boot = Boot();
  ASSERT_EQ(boot.staged.size(), 2u);
  const MountedMap& staged = boot.staged[0];
  EXPECT_EQ(staged.id, "alpha");
  EXPECT_EQ(staged.version, "1.0.0");
  EXPECT_EQ(staged.index_sha256, channel.IndexSha("alpha"));
  EXPECT_EQ(staged.platform, kPlatformPc);
  EXPECT_EQ(staged.source_sha256, Sha("source alpha"));
  EXPECT_EQ(staged.title, "Title alpha");
  EXPECT_EQ(staged.group, kGroup);
  EXPECT_EQ(staged.level, EchoVR::CSymbol64Hash("alpha"));
  ASSERT_EQ(boot.listed.size(), 3u);
  EXPECT_EQ(boot.listed[2], std::make_pair(EchoVR::CSymbol64Hash("other"), std::string("other")));
  EXPECT_EQ(boot.not_mounted, (std::vector<std::pair<std::string, std::string>>{{"beta", "local_map"}}));
}

TEST_F(MapRunTest, AChannelSwitchKeepsBothFloors) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(42);
  Fetch();
  channel.name = "echo-other";
  channel.Sign(7);
  config.channel = "echo-other";
  EXPECT_EQ(Fetch().reason, "");
  const CacheIndex index = Index();
  EXPECT_EQ(index.floors.channels.at("echo-test").at(kGroup).at(kKeyId).serial, 42u);
  EXPECT_EQ(index.floors.channels.at("echo-other").at(kGroup).at(kKeyId).serial, 7u);

  config.channel.clear();  // bound on first use: base_url now names echo-other, so echo-test's manifest is refused
  channel.name = "echo-test";
  channel.Sign(43);
  EXPECT_EQ(Fetch().reason, "wrong_channel");
}

TEST_F(MapRunTest, RevokedByIdByIndexShaAndByPieceShaAreNotMounted) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  const Package gamma = channel.Publish("gamma", "1.0.0", MakeSet(2, 'c'));
  channel.Publish("plain", "1.0.0", MakeSet(2, 'p'));
  channel.Sign(1);
  Fetch();
  // Revoked in the stored manifest (written by a fetch whose plan the revocation also reaches).
  channel.Sign(2, "2027-01-01T00:00:00Z", {"alpha"}, {channel.IndexSha("beta"), gamma.members[0].pieces[1].sha256});
  Fetch();
  const MountReport boot = Boot();
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"plain"}));
  EXPECT_EQ(TokenOf(boot, "alpha"), "revoked");
  EXPECT_EQ(TokenOf(boot, "beta"), "revoked");
  EXPECT_EQ(TokenOf(boot, "gamma"), "revoked");
}

TEST_F(MapRunTest, ExpiredTheScriptFreeMapMountsAndTheScriptedOneDoesNot) {
  channel.Publish("plain", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("scripted", "1.0.0", MakeSet(2, 'b'), {{1, "dll"}});
  channel.Sign(1, "2026-12-01T00:00:00Z");
  Fetch();
  const MountReport boot = Boot(nullptr, "2026-12-01T00:00:00Z");
  EXPECT_TRUE(Mounted(boot, "plain"));
  EXPECT_EQ(TokenOf(boot, "scripted"), "expired");
}

TEST_F(MapRunTest, AReplayedOldSerialIsRefusedAndTheStoredManifestIsKept) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  const std::string old_manifest = channel.Sign(4);
  const std::string current = channel.Sign(5);
  Fetch();
  env.server[channel.ManifestUrl()].body = old_manifest;
  const FetchReport replay = Fetch();
  EXPECT_EQ(replay.reason, "serial_down");
  EXPECT_EQ(replay.outcome, FetchOutcome::kFailed);
  std::string stored;
  ASSERT_TRUE(env.ReadSmallFile(StoredGroupManifestPath(kDir, kGroup), &stored, 1 << 20));
  EXPECT_EQ(stored, current);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, AnUpdateThatChangesOneFrameFetchesOnePieceAndReusesTheRest) {
  const std::string v1 = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", v1);
  channel.Sign(1);
  Fetch();
  std::string v2 = v1;
  v2[FramesStart(6) + 3 * kFrameBytes + 20] ^= 1;  // a byte of frame 3's payload
  channel.Publish("alpha", "1.1.0", v2);
  channel.Sign(2);
  env.hashed_bytes = 0;
  const FetchReport update = Fetch();
  EXPECT_EQ(update.maps_updated, 1u);
  EXPECT_EQ(update.pieces_fetched, 1u);
  EXPECT_EQ(update.pieces_reused, 6u);
  // A reused piece is copied, not hashed again: only the fetched piece and the package index are hashed.
  EXPECT_EQ(env.hashed_bytes, kFrameBytes + env.server[std::string(kBase) + "blobs/sha256/" + channel.IndexSha("alpha")].body.size());
  EXPECT_EQ(Boot().staged.at(0).version, "1.1.0");
}

// A cached file whose stamp moved since its bytes were verified is not trusted for any piece it holds: they are fetched.
TEST_F(MapRunTest, APieceInACachedFileWhoseStampMovedIsFetched) {
  const std::string v1 = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", v1);
  channel.Sign(1);
  Fetch();
  const std::string set_path = channel.VersionDir("alpha") + "/" + kSetPath;
  std::string altered = v1;
  altered[FramesStart(6) + 30] ^= 1;  // frame 0 on disk
  fs.Put(set_path, altered);
  std::string v2 = v1;
  v2[FramesStart(6) + 5 * kFrameBytes + 30] ^= 1;  // the update changes frame 5
  channel.Publish("alpha", "1.1.0", v2);
  channel.Sign(2);
  const FetchReport update = Fetch();
  EXPECT_EQ(update.pieces_fetched, 7u);
  EXPECT_EQ(update.pieces_reused, 0u);
  EXPECT_EQ(Boot().staged.at(0).version, "1.1.0");
}

TEST_F(MapRunTest, AStampMismatchIsDeferredNotHashedAndTheRehashRefreshesIt) {
  const std::string set_bytes = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", set_bytes);
  channel.Sign(1);
  Fetch();
  fs.Touch(channel.VersionDir("alpha") + "/" + kSetPath);
  env.hashed_bytes = 0;
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "stamp");
  EXPECT_LT(env.hashed_bytes, set_bytes.size());
  EXPECT_TRUE(mounter.calls.empty());
  EXPECT_EQ(env.open_blobs(), 0u);

  Fetch(nullptr, boot.deferred);
  EXPECT_NE(env.FindRecord("map_rehash", "result", "refreshed"), nullptr);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, ARehashThatFindsOtherBytesDropsTheVersionAndRefetchesIt) {
  const std::string set_bytes = MakeSet(3, 'a');
  channel.Publish("alpha", "1.0.0", set_bytes);
  channel.Sign(1);
  Fetch();
  std::string altered = set_bytes;
  altered[FramesStart(3) + 40] ^= 1;
  fs.Put(channel.VersionDir("alpha") + "/" + kSetPath, altered);
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "stamp");
  const FetchReport repair = Fetch(nullptr, boot.deferred);
  EXPECT_NE(env.FindRecord("map_rehash", "result", "dropped"), nullptr);
  EXPECT_EQ(repair.maps_updated, 1u);
  EXPECT_EQ(repair.pieces_fetched, 4u);  // the altered file's stamp moved, so none of its pieces is copied
  mounter.calls.clear();
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

// ADR 0048 decision 4, ADR 0045 decision 7: the mount holds a package index to its pin by the stamp recorded when the fetch hashed it,
// and hashes nothing. Bytes replaced under an unchanged stamp are read as the ones verified: a mount that hashed the index would
// find index_pin and defer the map instead. No hasher the env makes is fed a byte.
TEST_F(MapRunTest, TheMountHoldsThePackageIndexToItsPinByItsStampAndHashesNothing) {
  channel.Publish("alpha", "1.0.0", MakeSet(3, 'a'));
  channel.Sign(1);
  Fetch();
  const std::string package = channel.VersionDir("alpha") + "/package.json";
  std::string& bytes = env.fs_->files.at(package);
  const std::string key = "\"source_sha256\":\"";
  const std::size_t at = bytes.find(key);
  ASSERT_NE(at, std::string::npos);
  const std::size_t last = at + key.size() + 63;  // the source sha's last hex digit; the same size, so the same stamp
  bytes[last] = bytes[last] == '0' ? '1' : '0';
  const std::string replaced_source = bytes.substr(at + key.size(), 64);
  env.hashed_bytes = 0;
  const MountReport boot = Boot();
  ASSERT_TRUE(Mounted(boot, "alpha")) << TokenOf(boot, "alpha");
  EXPECT_EQ(boot.staged.at(0).source_sha256, replaced_source) << "the mount read the bytes on disk, held to the pin by the stamp";
  EXPECT_EQ(env.hashed_bytes, 0u);
  EXPECT_EQ(env.open_blobs(), Index().maps.at("alpha/windows-x86_64").files.size()) << "the index's handle is closed again";
}

// A package index whose stamp moved defers the map to the worker, which hashes the index again: equal bytes record the new stamp and
// the next mount takes the map.
TEST_F(MapRunTest, AMovedPackageIndexStampDefersTheMapAndTheRehashSettlesIt) {
  channel.Publish("alpha", "1.0.0", MakeSet(3, 'a'));
  channel.Sign(1);
  Fetch();
  const std::string package = channel.VersionDir("alpha") + "/package.json";
  fs.Touch(package);
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "package_stamp");
  EXPECT_TRUE(mounter.calls.empty());
  Fetch(nullptr, boot.deferred);
  EXPECT_NE(env.FindRecord("map_rehash", "result", "refreshed"), nullptr);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

// An index entry whose package stamp was never taken (all zero) holds nothing to: the mount defers rather than trust the bytes.
TEST_F(MapRunTest, ANeverStampedPackageIndexDefersTheMap) {
  channel.Publish("alpha", "1.0.0", MakeSet(3, 'a'));
  channel.Sign(1);
  Fetch();
  CacheIndex index = Index();
  index.maps.at("alpha/windows-x86_64").index_stamp = FileStamp();
  ASSERT_TRUE(WriteCacheIndex(env, kDir, index));
  EXPECT_EQ(TokenOf(Boot(), "alpha"), "package_stamp");
}

TEST_F(MapRunTest, APackageIndexWithOtherBytesIsDroppedByTheRehashAndFetchedAgain) {
  channel.Publish("alpha", "1.0.0", MakeSet(3, 'a'));
  channel.Sign(1);
  Fetch();
  const std::string package = channel.VersionDir("alpha") + "/package.json";
  std::string other = env.fs_->files.at(package);
  other += " ";
  fs.Put(package, other);
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "package_stamp");  // other bytes move the stamp; the mount defers, it does not hash
  const FetchReport repair = Fetch(nullptr, boot.deferred);
  EXPECT_NE(env.FindRecord("map_rehash", "reason", "index_hash"), nullptr);
  EXPECT_EQ(repair.maps_updated, 1u);
  mounter.calls.clear();
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

// ADR 0045 decision 8: what a fetch adds to the disk is a share of the free space the device reports, never a constant.
TEST_F(MapRunTest, AMapOverTheStorageShareIsSkippedAndTheOthersContinue) {
  channel.Publish("alpha", "1.0.0", MakeSet(10, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  channel.Sign(1);
  const std::uint64_t beta = MakeSet(2, 'b').size();
  std::uint64_t free_bytes = beta;
  while (free_bytes - free_bytes / nevr_cdn::kStorageShareKept < beta) ++free_bytes;
  env.device.free_bytes = free_bytes;
  const FetchReport fetched = Fetch();
  EXPECT_NE(env.FindRecord("map_plan", "reason", "over_budget"), nullptr);
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(fetched.reason, "over_budget");
  EXPECT_EQ(fetched.maps_updated, 1u);
  const MountReport boot = Boot();
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"beta"}));
}

// A fact the device did not give stops the fetch with its name, before any request.
TEST_F(MapRunTest, AMissingDeviceFactStopsTheFetchWithItsName) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  env.device.have_free = false;
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(fetched.reason, "no_free_space_fact");
  EXPECT_TRUE(env.requests.empty());
  EXPECT_NE(env.FindRecord("map_limits", "reason", "no_free_space_fact"), nullptr);
}

TEST_F(MapRunTest, MembersOverTheEntrysBytesAreRefusedBeforeAnyFetch) {
  channel.Publish("alpha", "1.0.0", MakeSet(10, 'a'));
  channel.maps["alpha"]["platforms"][kPlatformPc]["bytes"] = 1;
  channel.Sign(1);
  Fetch();
  EXPECT_NE(env.FindRecord("map_index", "reason", "bytes_mismatch"), nullptr);
  EXPECT_TRUE(Boot().mounted.empty());
}

TEST_F(MapRunTest, AMissingIndexMountsNothing) {
  const MountReport boot = Boot();
  EXPECT_TRUE(boot.mounted.empty());
  EXPECT_NE(env.FindRecord("map_mount_group", "reason", "no_index"), nullptr);
  EXPECT_TRUE(mounter.calls.empty());
}

TEST_F(MapRunTest, AMalformedIndexIsKeptAsBadAndTreatedAsEmpty) {
  fs.Put(std::string(kDir) + "/index.json", "{not json");
  EXPECT_TRUE(Boot().mounted.empty());
  EXPECT_NE(env.FindRecord("map_mount_group", "index", "invalid"), nullptr);
  EXPECT_TRUE(fs.Has(std::string(kDir) + "/index.json.bad"));
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  EXPECT_EQ(Fetch().maps_updated, 1u);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, NoFileIdDefersWithStampUnsupported) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  Fetch();
  fs.stamps_supported = false;
  EXPECT_EQ(TokenOf(Boot(), "alpha"), "stamp_unsupported");
  EXPECT_EQ(env.open_blobs(), 0u);
}

TEST_F(MapRunTest, AShutdownMidDownloadKeepsItsPiecesAndTheNextPassResumes) {
  const std::size_t pieces = PieceCount(channel.Publish("alpha", "1.0.0", MakeSet(6, 'a')));
  channel.Sign(1);
  int gets = 0;
  env.allow_get = [&] {
    if (++gets > 4) keep = false;  // shutdown after the manifest, the index and two pieces
    return true;
  };
  EXPECT_EQ(Fetch().reason, "cancelled");
  const std::string staging = channel.VersionDir("alpha") + ".tmp";
  EXPECT_TRUE(fs.HasDir(staging));
  EXPECT_TRUE(fs.Has(staging + "/resume/state.json"));
  EXPECT_EQ(Boot().mounted.size(), 0u);

  env.allow_get = nullptr;
  keep = true;
  const FetchReport resumed = Fetch();
  EXPECT_EQ(resumed.maps_updated, 1u);
  EXPECT_EQ(resumed.pieces_reused, 2u);
  EXPECT_EQ(resumed.pieces_fetched, pieces - 2);
  EXPECT_FALSE(fs.HasDir(staging));
  EXPECT_EQ(Boot().staged.at(0).version, "1.0.0");
}

// Quest asks every GET with a range from byte 0, which a CDN answers 206 with the whole object: it is accepted as 200 is.
TEST_F(MapRunTest, APartialContentAnswerCarryingTheWholeObjectIsAccepted) {
  channel.Publish("alpha", "1.0.0", MakeSet(3, 'a'));
  channel.Sign(1);
  for (auto& served : env.server) served.second.status = nevr_cdn::kHttpPartialContent;
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFetched);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, APieceLostInTransportKeepsTheOthersAndTheNextPassFetchesOnlyWhatIsMissing) {
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(6, 'a'));
  const std::size_t pieces = PieceCount(p);
  ASSERT_GE(p.members[0].pieces.size(), 3u);
  const std::string url = std::string(kBase) + "blobs/sha256/" + p.members[0].pieces[2].sha256;
  channel.Sign(1);
  env.server[url].status = 503;
  const FetchReport lost = Fetch();
  EXPECT_EQ(lost.maps_updated, 0u);
  EXPECT_EQ(lost.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(lost.reason, "http_503");
  EXPECT_NE(env.FindRecord("map_piece", "reason", "http_503"), nullptr);
  const std::string staging = channel.VersionDir("alpha") + ".tmp";
  EXPECT_TRUE(fs.HasDir(staging));

  env.server[url].status = 200;
  const FetchReport resumed = Fetch();
  EXPECT_EQ(resumed.maps_updated, 1u);
  EXPECT_EQ(resumed.pieces_reused, 2u);
  EXPECT_EQ(resumed.pieces_fetched, pieces - 2);
  EXPECT_FALSE(fs.HasDir(staging));
}

// The kept bytes are trusted only while the staging file's stamp is the one recorded when the transfer stopped.
TEST_F(MapRunTest, AStagedMemberAlteredOnDiskIsFetchedAgainFromItsStart) {
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(6, 'a'));
  const std::size_t pieces = PieceCount(p);
  channel.Sign(1);
  int gets = 0;
  env.allow_get = [&] {
    if (++gets > 4) keep = false;
    return true;
  };
  EXPECT_EQ(Fetch().reason, "cancelled");
  const std::string staged = channel.VersionDir("alpha") + ".tmp/" + kSetPath;
  ASSERT_TRUE(fs.Has(staged));
  std::string bytes = env.fs_->files[staged];
  bytes[0] ^= 1;
  fs.Put(staged, bytes);

  env.allow_get = nullptr;
  keep = true;
  const FetchReport resumed = Fetch();
  EXPECT_EQ(resumed.maps_updated, 1u);
  EXPECT_EQ(resumed.pieces_reused, 0u);
  EXPECT_EQ(resumed.pieces_fetched, pieces);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, AnUnfinishedDownloadOfASupersededVersionIsRemoved) {
  const std::string v1 = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", v1);
  channel.Sign(1);
  int gets = 0;
  env.allow_get = [&] {
    if (++gets > 4) keep = false;
    return true;
  };
  EXPECT_EQ(Fetch().reason, "cancelled");
  const std::string old_staging = channel.VersionDir("alpha") + ".tmp";
  ASSERT_TRUE(fs.HasDir(old_staging));

  std::string v2 = v1;
  v2[FramesStart(6) + 3 * kFrameBytes + 20] ^= 1;
  channel.Publish("alpha", "1.1.0", v2);
  channel.Sign(2);
  env.allow_get = nullptr;
  keep = true;
  EXPECT_EQ(Fetch().maps_updated, 1u);
  EXPECT_FALSE(fs.HasDir(old_staging));
  EXPECT_NE(env.FindRecord("map_gc", "reason", "superseded"), nullptr);
  EXPECT_EQ(Boot().staged.at(0).version, "1.1.0");
}

TEST_F(MapRunTest, APieceHashMismatchIsRetriedThreeTimesThenFailsTheMap) {
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  const std::string url = std::string(kBase) + "blobs/sha256/" + p.members[0].pieces[1].sha256;
  env.corrupt_times[url] = 3;
  EXPECT_EQ(Fetch().maps_updated, 1u);
  EXPECT_EQ(GetsOf(url), 4u);

  const Package& q = channel.Publish("beta", "1.0.0", MakeSet(2, 'q'));  // frames alpha does not hold
  channel.Sign(2);
  const std::string bad = std::string(kBase) + "blobs/sha256/" + q.members[0].pieces[1].sha256;
  env.corrupt_times[bad] = 4;
  const FetchReport failed = Fetch();
  EXPECT_EQ(failed.maps_updated, 0u);
  EXPECT_EQ(failed.reason, "piece_hash");
  EXPECT_NE(env.FindRecord("map_piece", "reason", "piece_hash"), nullptr);
  EXPECT_FALSE(fs.HasDir(channel.VersionDir("beta") + ".tmp"));
}

TEST_F(MapRunTest, AMapTheGroupNoLongerListsIsRemovedByTheNextGc) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  Fetch();
  channel.Unpublish("alpha");
  channel.Sign(2);
  Fetch();  // the group's list loses alpha at the end of this pass
  Fetch();  // and this pass's GC removes it
  EXPECT_FALSE(fs.HasDir(std::string(kDir) + "/alpha"));
  EXPECT_TRUE(Index().maps.empty());
}

// The audit's scenario: testers trusted a test key on channel echo-test, then switch to the official key, whose manifest carries
// the same channel name with a lower serial and a lower map version.
TEST_F(MapRunTest, SwitchingToTheOfficialKeyStartsItsOwnFloorAndSaysSoOnce) {
  channel.Publish("alpha", "1.3.0", MakeSet(2, 'a'));
  channel.Sign(40);
  ASSERT_EQ(Fetch().reason, "");

  TestKey official_key{2, true};
  Channel official{env, official_key};
  official.Publish("alpha", "1.0.0", MakeSet(2, 'b'));
  official.Sign(1);
  config.keys = {official_key.pin};
  EXPECT_EQ(Fetch().reason, "");
  EXPECT_EQ(Index().maps.at(MapKey("alpha", kPlatformPc)).version, "1.0.0");
  const Record* changed = env.FindRecord("map_trust_changed");
  ASSERT_NE(changed, nullptr);
  EXPECT_EQ(changed->Get("untrusted_keys"), kKeyId);
  EXPECT_EQ(changed->Get("key_id"), "A9A8A7A6A5A4A3A2");
  EXPECT_EQ(changed->Get("channel"), "echo-test");
  EXPECT_EQ(changed->Get("group"), kGroup);
  EXPECT_TRUE(Boot().mounted == std::vector<std::string>{"alpha"});

  official.Sign(2);
  EXPECT_EQ(Fetch().reason, "");
  official.Sign(1);
  EXPECT_EQ(Fetch().reason, "serial_down");  // a rollback under the same key
  EXPECT_EQ(env.CountRecords("map_trust_changed"), 1u);
}

// ---- Groups (ADR 0048): one manifest per lobby group, keyed by its opaque id; the cache keeps the union of the groups' maps.

// A fetch keyed by one group requests that group's manifest and the objects of its maps, and nothing of another group.
TEST_F(MapRunTest, AGroupsFetchRequestsOnlyThatGroupsManifestAndObjects) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  TestKey other_key{1};
  Channel other{env, other_key};
  other.group = kOtherGroup;
  const Package& beta = other.Publish("beta", "1.0.0", MakeSet(2, 'q'));  // frames alpha does not hold
  other.Sign(1);

  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFetched);
  EXPECT_EQ(fetched.group, kGroup);
  EXPECT_EQ(GetsOf(channel.ManifestUrl()), 1u);
  EXPECT_EQ(GetsOf(other.ManifestUrl()), 0u);
  EXPECT_EQ(GetsUnder(std::string(kBase) + "guilds/" + kOtherGroup), 0u);
  EXPECT_EQ(GetsOf(std::string(kBase) + "blobs/sha256/" + other.IndexSha("beta")), 0u);
  // Every piece but the first: two sets of one size share their header and tables, which alpha's fetch got.
  for (std::size_t i = 1; i < beta.members[0].pieces.size(); ++i) {
    EXPECT_EQ(GetsOf(std::string(kBase) + "blobs/sha256/" + beta.members[0].pieces[i].sha256), 0u);
  }
  EXPECT_EQ(Index().maps.count(MapKey("beta", kPlatformPc)), 0u);
  EXPECT_EQ(CachedGroupIds(env, kDir), (std::vector<std::string>{kGroup}));
}

TEST_F(MapRunTest, AManifestThatNamesAnotherGroupIsRefused) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1, "2027-01-01T00:00:00Z", {}, {}, kOtherGroup);
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(fetched.reason, "guild_mismatch");
  EXPECT_FALSE(fs.Has(StoredGroupManifestPath(kDir, kGroup)));
  EXPECT_TRUE(Index().maps.empty());
  EXPECT_NE(env.FindRecord("map_manifest", "reason", "guild_mismatch"), nullptr);

  // A stored copy under the wrong group is refused by the mount too.
  channel.Sign(1);
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  fs.Put(StoredGroupManifestPath(kDir, kOtherGroup), env.fs_->files[StoredGroupManifestPath(kDir, kGroup)]);
  const MountReport boot = Boot(nullptr, kNow, {kOtherGroup});
  EXPECT_TRUE(boot.mounted.empty());
  EXPECT_EQ(boot.groups_refused, (std::vector<std::pair<std::string, std::string>>{{kOtherGroup, "guild_mismatch"}}));
}

TEST_F(MapRunTest, AGroupIdThatIsNotPathSafeIsRefusedBeforeAnyRequest) {
  const FetchReport fetched = Fetch(nullptr, {}, "../escape");
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(fetched.reason, "bad_group");
  EXPECT_TRUE(env.requests.empty());
}

// A map two groups list at one version is fetched once: the second group's fetch finds it cached.
TEST_F(MapRunTest, AMapTwoGroupsListIsFetchedOnce) {
  const std::string set_bytes = MakeSet(3, 'a');
  channel.Publish("alpha", "1.0.0", set_bytes);
  channel.Sign(1);
  Channel other{env, key};
  other.group = kOtherGroup;
  other.Publish("alpha", "1.0.0", set_bytes);
  other.Sign(1);

  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  const std::size_t blob_gets = GetsUnder(std::string(kBase) + "blobs/");
  const FetchReport second = Fetch(nullptr, {}, kOtherGroup);
  EXPECT_EQ(second.outcome, FetchOutcome::kCached);
  EXPECT_EQ(GetsUnder(std::string(kBase) + "blobs/"), blob_gets);
  EXPECT_NE(env.FindRecord("map_plan", "reason", "current"), nullptr);
  const MountReport boot = Boot(nullptr, kNow, {kOtherGroup, kGroup});
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"alpha"}));
}

// A piece two maps share is fetched once: the second map copies it from the first map's stored file.
TEST_F(MapRunTest, APieceTwoMapsShareIsFetchedOnce) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "shared dll"}});
  channel.Publish("beta", "1.0.0", MakeSet(2, 'q'), {{2, "shared dll"}});
  channel.Sign(1);
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.maps_updated, 2u);
  EXPECT_EQ(GetsOf(std::string(kBase) + "blobs/sha256/" + Sha("shared dll")), 1u);
  EXPECT_EQ(fetched.pieces_reused, 2u);  // the script, and the sets' header and tables, which the two sets share
}

// The client's source: the groups it fetched are kept in its cache index, the latest first, and a launch mounts and fetches
// them again from there. No config key names a group.
TEST_F(MapRunTest, TheClientKeepsTheGroupsItFetchedTheLatestFirst) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  Channel other{env, key};
  other.group = kOtherGroup;
  other.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  other.Sign(1);
  Fetch();
  Fetch(nullptr, {}, kOtherGroup);
  EXPECT_EQ(CachedGroupIds(env, kDir), (std::vector<std::string>{kOtherGroup, kGroup}));
  Fetch();  // played in the first group again: it becomes the latest
  const std::vector<std::string> groups = CachedGroupIds(env, kDir);
  EXPECT_EQ(groups, (std::vector<std::string>{kGroup, kOtherGroup}));

  const MountReport boot = Boot(nullptr, kNow, groups);
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"alpha", "beta"}));

  env.requests.clear();
  for (const std::string& group : groups) EXPECT_EQ(Fetch(nullptr, {}, group).outcome, FetchOutcome::kCached);
  EXPECT_EQ(env.requests, (std::vector<std::string>{channel.ManifestUrl(), other.ManifestUrl()}));
}

// Two groups list one map at different versions: the cache holds the version of the group fetched last, and a mount, whatever
// the order of its groups, mounts that version under the group whose manifest pins it.
TEST_F(MapRunTest, TheGroupFetchedLastDecidesAMapsVersion) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  Channel other{env, key};
  other.group = kOtherGroup;
  other.Publish("alpha", "1.1.0", MakeSet(2, 'b'));
  other.Sign(1);
  Fetch();
  Fetch(nullptr, {}, kOtherGroup);
  EXPECT_EQ(Index().maps.at(MapKey("alpha", kPlatformPc)).version, "1.1.0");
  for (const std::vector<std::string>& groups : {std::vector<std::string>{kOtherGroup, kGroup},
                                                 std::vector<std::string>{kGroup, kOtherGroup}}) {
    const MountReport boot = Boot(nullptr, kNow, groups);
    ASSERT_EQ(boot.staged.size(), 1u);
    EXPECT_EQ(boot.staged[0].version, "1.1.0");
    EXPECT_EQ(boot.staged[0].group, kOtherGroup);
  }
}

// How many groups the cache keeps is bounded by the device's storage share, the oldest dropped first; the maps only a dropped
// group listed go with it.
TEST_F(MapRunTest, TheOldestGroupIsDroppedWhenTheStorageShareCannotHoldIt) {
  constexpr int kChunks = 40;  // a set larger than its group's manifest, so the maps decide what a group costs
  channel.Publish("alpha", "1.0.0", MakeSet(kChunks, 'a'));
  channel.Sign(1);
  Channel other{env, key};
  other.group = kOtherGroup;
  other.Publish("beta", "1.0.0", MakeSet(kChunks, 'b'));
  other.Sign(1);
  constexpr const char* kThirdGroup = "third-group";
  Channel third{env, key};
  third.group = kThirdGroup;
  third.Sign(1);  // lists no map
  Fetch();
  Fetch(nullptr, {}, kOtherGroup);

  env.device.free_bytes = 1;  // what the cache holds now, and almost nothing more
  const FetchReport fetched = Fetch(nullptr, {}, kThirdGroup);
  EXPECT_EQ(fetched.outcome, FetchOutcome::kCached);
  EXPECT_EQ(CachedGroupIds(env, kDir), (std::vector<std::string>{kThirdGroup, kOtherGroup}));
  EXPECT_NE(env.FindRecord("map_gc", "group", kGroup), nullptr);
  EXPECT_FALSE(fs.HasDir(std::string(kDir) + "/alpha"));
  EXPECT_TRUE(fs.HasDir(std::string(kDir) + "/beta"));
  EXPECT_FALSE(fs.Has(StoredGroupManifestPath(kDir, kGroup)));
}

// The mount can run again (after a fetch, off the boot thread): a map it mounted is kept, not granted twice, and a map that
// arrived since is mounted.
TEST_F(MapRunTest, TheMountCanRunAgainAndMountsOnlyWhatArrived) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Sign(1);
  Fetch();
  CacheMount mount(env, fs, config, mounter, nullptr);
  const MountReport first = mount.Mount({kGroup}, kNow);
  EXPECT_EQ(first.mounted, (std::vector<std::string>{"alpha"}));
  const std::size_t grants = mounter.calls.size();

  MountReport again = mount.Mount({kGroup}, kNow);
  EXPECT_TRUE(again.mounted.empty());
  EXPECT_EQ(again.kept, (std::vector<std::string>{"alpha"}));
  EXPECT_EQ(mounter.calls.size(), grants);
  ExpectNoLeak();

  Channel other{env, key};
  other.group = kOtherGroup;
  other.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  other.Sign(1);
  Fetch(nullptr, {}, kOtherGroup);
  again = mount.Mount({kOtherGroup, kGroup}, kNow);
  EXPECT_EQ(again.mounted, (std::vector<std::string>{"beta"}));
  EXPECT_EQ(again.kept, (std::vector<std::string>{"alpha"}));
  EXPECT_FALSE(mounter.Called("script map:beta 1"));

  // A version that arrived for a mounted map is not swapped in this process.
  channel.Publish("alpha", "1.1.0", MakeSet(2, 'c'), {{1, "dll"}});
  channel.Sign(2);
  Fetch();
  again = mount.Mount({kGroup}, kNow);
  EXPECT_EQ(TokenOf(again, "alpha"), "mounted_other_version");
}

// The mount holds every file for the process: the files it takes are bounded by a share of the descriptors the device reports.
TEST_F(MapRunTest, TheMountHoldsNoMoreFilesThanTheDescriptorShare) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll one"}, {2, "dll two"}});
  channel.Sign(1);
  Fetch();
  constexpr std::uint64_t kFilesInShare = 2;  // alpha holds 3: its set and two scripts
  env.device.fds_open = env.device.fd_soft_limit - kFilesInShare * kDescriptorShareForMaps;
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "descriptors");
  EXPECT_EQ(env.open_blobs(), 0u);
  env.device.have_fds = false;
  EXPECT_TRUE(Boot().mounted.empty());
  EXPECT_NE(env.FindRecord("map_mount_group", "reason", "no_descriptor_fact"), nullptr);
}

// The size of the set before any piece, and the bytes fetched with the time taken as the fetch goes: what a caller needs to bound
// a wait by the download rate.
TEST_F(MapRunTest, TheFetchReportsTheSetsSizeFirstAndThenItsProgress) {
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Sign(1);
  std::vector<FetchProgress> seen;
  std::size_t piece_gets_at_first = 0;
  const FetchReport fetched = Fetch(nullptr, {}, kGroup, [&](const FetchProgress& progress) {
    if (seen.empty()) piece_gets_at_first = GetsUnder(std::string(kBase) + "blobs/");
    seen.push_back(progress);
  });
  ASSERT_GE(seen.size(), 2u);
  EXPECT_EQ(seen.front().set_bytes, p.members[0].size);
  EXPECT_EQ(seen.front().bytes_to_fetch, p.members[0].size);
  EXPECT_EQ(seen.front().bytes_fetched, 0u);
  EXPECT_EQ(piece_gets_at_first, 0u);  // known from the manifest, before the index or any piece
  EXPECT_EQ(seen.back().bytes_fetched, p.members[0].size);
  for (std::size_t i = 1; i < seen.size(); ++i) {
    EXPECT_GE(seen[i].bytes_fetched, seen[i - 1].bytes_fetched);
    EXPECT_GT(seen[i].elapsed_ms, seen[i - 1].elapsed_ms);
  }
  EXPECT_EQ(fetched.set_bytes, p.members[0].size);
  EXPECT_EQ(fetched.bytes_fetched, p.members[0].size);
  EXPECT_GT(fetched.elapsed_ms, 0u);
}

// ---- The request queue (core/map_fetch_queue.h).

FetchReport Answer(const std::string& group, FetchOutcome outcome) {
  FetchReport r;
  r.group = group;
  r.outcome = outcome;
  return r;
}

TEST(GroupFetchQueue, ARequestIsAnsweredOnTheWorkerWithTheFetchsOutcome) {
  GroupFetchQueue queue;
  std::vector<std::string> fetched;
  std::vector<std::string> heard;
  std::string reason;
  ASSERT_TRUE(queue.Request(kGroup, [&](const FetchReport& r) { heard.push_back(r.group + ":" + FetchOutcomeName(r.outcome)); },
                            &reason));
  EXPECT_TRUE(queue.WaitForRequest(0));
  EXPECT_TRUE(queue.RunNext([&](const std::string& group) {
    fetched.push_back(group);
    return Answer(group, FetchOutcome::kCached);
  }));
  EXPECT_EQ(fetched, (std::vector<std::string>{kGroup}));
  EXPECT_EQ(heard, (std::vector<std::string>{std::string(kGroup) + ":cached"}));
  EXPECT_FALSE(queue.RunNext([&](const std::string& group) { return Answer(group, FetchOutcome::kFailed); }));
  EXPECT_FALSE(queue.WaitForRequest(0));
}

TEST(GroupFetchQueue, TwoRequestsForAWaitingGroupShareOneFetch) {
  GroupFetchQueue queue;
  int fetches = 0, heard = 0;
  std::string reason;
  for (int i = 0; i < 2; ++i) ASSERT_TRUE(queue.Request(kGroup, [&](const FetchReport&) { ++heard; }, &reason));
  ASSERT_TRUE(queue.Request(kOtherGroup, [&](const FetchReport&) { ++heard; }, &reason));
  EXPECT_EQ(queue.Waiting(), 2u);
  while (queue.RunNext([&](const std::string& group) {
    ++fetches;
    return Answer(group, FetchOutcome::kFetched);
  })) {
  }
  EXPECT_EQ(fetches, 2);
  EXPECT_EQ(heard, 3);
}

// A request that arrives while its group is being fetched waits for a fetch of its own: the running one may have read the
// manifest already.
TEST(GroupFetchQueue, ARequestDuringItsGroupsFetchGetsAFetchOfItsOwn) {
  GroupFetchQueue queue;
  int fetches = 0;
  std::string reason;
  ASSERT_TRUE(queue.Request(kGroup, nullptr, &reason));
  queue.RunNext([&](const std::string& group) {
    ++fetches;
    EXPECT_TRUE(queue.Request(group, nullptr, &reason));
    return Answer(group, FetchOutcome::kFetched);
  });
  EXPECT_EQ(queue.Waiting(), 1u);
  queue.RunNext([&](const std::string& group) {
    ++fetches;
    return Answer(group, FetchOutcome::kCached);
  });
  EXPECT_EQ(fetches, 2);
}

TEST(GroupFetchQueue, ABadGroupIsRefusedAndStopAnswersEveryWaitingRequest) {
  GroupFetchQueue queue;
  std::string reason;
  EXPECT_FALSE(queue.Request("Not/Safe", [](const FetchReport&) { FAIL() << "a refused request is never answered"; }, &reason));
  EXPECT_EQ(reason, "bad_group");
  std::vector<std::string> heard;
  ASSERT_TRUE(queue.Request(kGroup, [&](const FetchReport& r) { heard.push_back(FetchOutcomeName(r.outcome) + (":" + r.reason)); },
                            &reason));
  queue.Stop();
  EXPECT_EQ(heard, (std::vector<std::string>{"failed:cancelled"}));
  EXPECT_FALSE(queue.Request(kGroup, nullptr, &reason));
  EXPECT_EQ(reason, "stopped");
  EXPECT_FALSE(queue.WaitForRequest(0));
}

// ---- On demand (ADR 0048 decision 16): a client's fetch stores the maps it asks for and the ones it holds, never the rest of a group.

class OnDemandTest : public MapRunTest {
 protected:
  FetchReport FetchOnDemand(const std::vector<std::string>& ids) {
    FetchScope scope;
    scope.on_demand = true;
    scope.ids = ids;
    return FetchGroup(env, fs, config, kGroup, {}, nullptr, kNow, keep_going, nullptr, scope);
  }
  std::size_t IndexGets(const std::string& id) { return GetsOf(std::string(kBase) + "blobs/sha256/" + channel.IndexSha(id)); }
  // The request pass with the launch's mount. `unscoped` is production's route: MountScope is null (no platform sets mount_on_demand),
  // so the mount grants and reports every map of the group. Otherwise the mount is scoped to the ids the requests asked for.
  void ServeWithLaunchMount(MapRequests& requests, CacheMount& launch, bool unscoped) {
    nevr_map_channel::ServeSessionRequests(
        requests,
        [this](const std::string& group, const FetchScope& scope) {
          return FetchGroup(env, fs, config, group, {}, nullptr, kNow, keep_going, nullptr, scope);
        },
        [](const std::string&) { return MountReport{}; }, [](nevr_map_session_pin::ChannelView) {}, keep_going,
        [&launch, unscoped](const std::string& group, const std::vector<std::string>& asked) {
          return launch.Mount({group}, kNow, unscoped ? nullptr : &asked);
        });
  }
  // A map this launch mounted at 1.0.0, updated to 1.1.0 by a fetch; the request for it is served with the launch's mount.
  void SupersededRequestCase(bool unscoped) {
    channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
    channel.Sign(1);
    ASSERT_EQ(FetchOnDemand({"beta"}).maps_updated, 1u);  // the launch has beta to mount
    CacheMount launch(env, fs, config, mounter, nullptr);
    ASSERT_TRUE(Mounted(launch.Mount({kGroup}, kNow), "beta"));
    channel.Publish("beta", "1.1.0", MakeSet(4, 'c'));
    channel.Sign(2);
    MapRequests requests(env);
    std::string reason;
    ASSERT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;
    ASSERT_TRUE(requests.RequestMap("beta", &reason)) << reason;
    ServeWithLaunchMount(requests, launch, unscoped);
    const Record* answer = env.FindRecord("map_request", "id", "beta");
    ASSERT_NE(answer, nullptr);
    EXPECT_EQ(answer->Get("outcome"), "fetched");
    EXPECT_EQ(answer->Get("consequence"), "the update is stored and applies the next time you start the game");
  }
  // A map this launch did not have: the fetch stores it and the request's mount grants it, so it mounts in this run.
  void NewMapRequestCase(bool unscoped) {
    channel.Sign(1);
    CacheMount launch(env, fs, config, mounter, nullptr);
    EXPECT_TRUE(launch.Mount({kGroup}, kNow).mounted.empty());
    channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
    channel.Sign(2);
    MapRequests requests(env);
    std::string reason;
    ASSERT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;
    ASSERT_TRUE(requests.RequestMap("beta", &reason)) << reason;
    ServeWithLaunchMount(requests, launch, unscoped);
    const Record* answer = env.FindRecord("map_request", "id", "beta");
    ASSERT_NE(answer, nullptr);
    EXPECT_EQ(answer->Get("outcome"), "fetched");
    EXPECT_EQ(answer->Get("consequence"), "the map is stored and mounts during this run: join the session again when it is ready");
  }
  // The worker: runs each queued request with the fetch its scope asks for, until none waits.
  void Serve(MapRequests& requests) {
    while (requests.RunNext([this](const std::string& group, const FetchScope& scope) {
      return FetchGroup(env, fs, config, group, {}, nullptr, kNow, keep_going, nullptr, scope);
    })) {
    }
  }
};

TEST_F(OnDemandTest, OnlyTheNamedMapOfAGroupIsFetched) {
  channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
  channel.Sign(1);
  const FetchReport fetched = FetchOnDemand({"beta"});
  EXPECT_EQ(fetched.reason, "");
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFetched);
  EXPECT_EQ(fetched.maps_updated, 1u);
  EXPECT_EQ(IndexGets("alpha"), 0u);
  EXPECT_EQ(IndexGets("beta"), 1u);
  const MountReport boot = Boot();
  EXPECT_TRUE(Mounted(boot, "beta"));
  EXPECT_FALSE(Mounted(boot, "alpha"));
  EXPECT_EQ(TokenOf(boot, "alpha"), "not_cached");
}

TEST_F(OnDemandTest, ACachedCurrentMapCostsOnlyTheManifest) {
  channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Sign(1);
  ASSERT_EQ(FetchOnDemand({"alpha"}).outcome, FetchOutcome::kFetched);
  const std::size_t gets = env.requests.size();
  const FetchReport again = FetchOnDemand({"alpha"});
  EXPECT_EQ(again.outcome, FetchOutcome::kCached);
  EXPECT_EQ(again.pieces_fetched, 0u);
  EXPECT_EQ(env.requests.size(), gets + 1);  // the group's manifest only
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

// A held map the manifest updated is refreshed though nothing names it: only its changed piece is fetched.
TEST_F(OnDemandTest, AHeldMapTheManifestUpdatedFetchesOnlyTheChangedPieceAndMountsTheNewVersion) {
  const std::string v1 = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", v1);
  channel.Sign(1);
  ASSERT_EQ(FetchOnDemand({"alpha"}).outcome, FetchOutcome::kFetched);
  std::string v2 = v1;
  v2[FramesStart(6) + 3 * kFrameBytes + 20] ^= 1;  // a byte of frame 3's payload
  channel.Publish("alpha", "1.1.0", v2);
  channel.Sign(2);
  const FetchReport update = FetchOnDemand({});
  EXPECT_EQ(update.maps_updated, 1u);
  EXPECT_EQ(update.pieces_fetched, 1u);
  EXPECT_EQ(update.pieces_reused, 6u);
  const MountReport boot = Boot();
  ASSERT_EQ(boot.staged.size(), 1u);
  EXPECT_EQ(boot.staged[0].version, "1.1.0");
}

TEST_F(OnDemandTest, AnUpdateThatFailsToFetchLeavesTheOldVersionRefusedStale) {
  channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Sign(1);
  ASSERT_EQ(FetchOnDemand({"alpha"}).outcome, FetchOutcome::kFetched);
  channel.Publish("alpha", "1.1.0", MakeSet(4, 'b'));
  channel.Sign(2);
  const std::string v2_index = std::string(kBase) + "blobs/sha256/" + channel.IndexSha("alpha");
  env.server.erase(v2_index);  // the manifest arrives, the new version cannot be had
  const FetchReport failed = FetchOnDemand({"alpha"});
  EXPECT_EQ(failed.outcome, FetchOutcome::kFailed);
  const MountReport boot = Boot();
  EXPECT_TRUE(boot.staged.empty());
  EXPECT_EQ(TokenOf(boot, "alpha"), kStaleToken);
  EXPECT_FALSE(mounter.Called("set map:alpha"));
}

TEST_F(OnDemandTest, ANamedIdTheManifestDoesNotListFailsAndFetchesNothing) {
  channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Sign(1);
  const FetchReport fetched = FetchOnDemand({"ghost"});
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(fetched.reason, "not_listed");
  EXPECT_EQ(fetched.pieces_fetched, 0u);
  EXPECT_EQ(IndexGets("alpha"), 0u);
  EXPECT_NE(env.FindRecord("map_plan", "reason", "not_listed"), nullptr);
  EXPECT_TRUE(Boot().mounted.empty());
}

// The mount grants only the asked ids under mount_on_demand: every other cached map at its listed version is not_asked.
TEST_F(OnDemandTest, AMountGrantsOnlyTheAskedMapsAndTheFlagIsOffByDefault) {
  EXPECT_FALSE(ChannelConfig().mount_on_demand);
  channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
  channel.Sign(1);
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);  // both cached
  config.mount_on_demand = true;
  CacheMount mount(env, fs, config, mounter, nullptr);
  const std::vector<std::string> asked{"beta"};
  const MountReport boot = mount.Mount({kGroup}, kNow, &asked);
  EXPECT_TRUE(Mounted(boot, "beta"));
  EXPECT_FALSE(Mounted(boot, "alpha"));
  EXPECT_EQ(TokenOf(boot, "alpha"), kNotAskedToken);
  EXPECT_FALSE(mounter.Called("set map:alpha"));
}

// ---- The request entry (core/map_request.h): a session's group and the map it lacks, queued for the worker.

TEST_F(OnDemandTest, ASessionSuccessDownloadsNoMapOfItsGroup) {
  channel.Publish("arena_c", "1.0.0", MakeSet(4, 'c'));
  channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
  channel.Sign(1);
  MapRequests requests(env);
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;
  Serve(requests);
  EXPECT_EQ(requests.SessionGroup(), std::string(kGroup));
  EXPECT_EQ(IndexGets("arena_c"), 0u);
  EXPECT_EQ(IndexGets("beta"), 0u);
  EXPECT_NE(env.FindRecord("map_request", "outcome", "cached"), nullptr);
}

// One launch's request pass (ServeSessionRequests) for a map the launch already mounted: the request's fetch stores a newer version,
// and its mount, the launch's own CacheMount, finds the map mounted at the old one. The answer says the update waits for the next start.
TEST_F(OnDemandTest, ARequestForAMapMountedAtAnOlderVersionSaysTheUpdateWaitsForTheNextStart) {
  SupersededRequestCase(/*unscoped=*/false);
}

TEST_F(OnDemandTest, ARequestForAMapMountedAtAnOlderVersionSaysTheUpdateWaitsForTheNextStartOnTheUnscopedMount) {
  SupersededRequestCase(/*unscoped=*/true);
}

// The same pass for a map this launch did not have: the fetch stores it and the request's mount grants it, so it mounts in this run.
TEST_F(OnDemandTest, ARequestForANewMapSaysItMountsDuringThisRun) {
  NewMapRequestCase(/*unscoped=*/false);
}

TEST_F(OnDemandTest, ARequestForANewMapSaysItMountsDuringThisRunOnTheUnscopedMount) {
  NewMapRequestCase(/*unscoped=*/true);
}

// Production's route mounts and reports the whole group. A named request for a map that is current must not be told about a sibling
// the mount found mounted at another version: alpha's copy is the current version, and beta's update is the one that waits.
TEST_F(OnDemandTest, ACurrentMapIsNotToldOfASiblingSupersededByTheMount) {
  channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
  channel.Sign(1);
  ASSERT_EQ(FetchOnDemand({"alpha", "beta"}).maps_updated, 2u);
  CacheMount launch(env, fs, config, mounter, nullptr);
  const MountReport boot = launch.Mount({kGroup}, kNow);
  ASSERT_TRUE(Mounted(boot, "alpha"));
  ASSERT_TRUE(Mounted(boot, "beta"));
  channel.Publish("beta", "1.1.0", MakeSet(4, 'c'));
  channel.Sign(2);
  ASSERT_EQ(FetchOnDemand({"beta"}).maps_updated, 1u);  // beta 1.1.0 is stored; the launch still holds beta 1.0.0
  MapRequests requests(env);
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;
  ASSERT_TRUE(requests.RequestMap("alpha", &reason)) << reason;
  ServeWithLaunchMount(requests, launch, /*unscoped=*/true);
  const Record* answer = env.FindRecord("map_request", "id", "alpha");
  ASSERT_NE(answer, nullptr);
  EXPECT_EQ(answer->Get("outcome"), "cached");
  EXPECT_EQ(answer->Get("consequence"), "your copy is the current version");
}

// Production's route again, with the sibling the other way round: beta is stored but not yet mounted, and the request's mount grants it
// in this run. A named request for alpha, which is current, must not be told that alpha mounts during this run.
TEST_F(OnDemandTest, ACurrentMapIsNotToldOfASiblingGrantedByTheMount) {
  channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Sign(1);
  ASSERT_EQ(FetchOnDemand({"alpha"}).maps_updated, 1u);
  CacheMount launch(env, fs, config, mounter, nullptr);
  ASSERT_TRUE(Mounted(launch.Mount({kGroup}, kNow), "alpha"));
  channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
  channel.Sign(2);
  ASSERT_EQ(FetchOnDemand({"beta"}).maps_updated, 1u);  // beta is stored, not mounted by this launch
  MapRequests requests(env);
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;
  ASSERT_TRUE(requests.RequestMap("alpha", &reason)) << reason;
  ServeWithLaunchMount(requests, launch, /*unscoped=*/true);
  EXPECT_TRUE(mounter.Called("set map:beta"));  // the request's mount did grant beta in this run
  const Record* answer = env.FindRecord("map_request", "id", "alpha");
  ASSERT_NE(answer, nullptr);
  EXPECT_EQ(answer->Get("outcome"), "cached");
  EXPECT_EQ(answer->Get("consequence"), "your copy is the current version");
}

// A group request names no map, so its answer keeps everything its mount did: beta, stored and granted in this run, is told.
TEST_F(OnDemandTest, AGroupRequestKeepsEveryMapItsMountGrants) {
  channel.Sign(1);
  CacheMount launch(env, fs, config, mounter, nullptr);
  EXPECT_TRUE(launch.Mount({kGroup}, kNow).mounted.empty());
  channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
  channel.Sign(2);
  ASSERT_EQ(FetchOnDemand({"beta"}).maps_updated, 1u);  // beta is stored, not mounted by this launch
  MapRequests requests(env);
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;
  ServeWithLaunchMount(requests, launch, /*unscoped=*/true);
  EXPECT_TRUE(mounter.Called("set map:beta"));
  const Record* answer = env.FindRecord("map_request", "id", "-");
  ASSERT_NE(answer, nullptr);
  EXPECT_EQ(answer->Get("consequence"), "the map is stored and mounts during this run: join the session again when it is ready");
}

TEST_F(OnDemandTest, ARequestedMapIsFetchedAndTheOthersAreNot) {
  channel.Publish("arena_c", "1.0.0", MakeSet(4, 'c'));
  channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
  channel.Sign(1);
  MapRequests requests(env);
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;
  ASSERT_TRUE(requests.RequestMap("beta", &reason)) << reason;
  Serve(requests);
  EXPECT_EQ(IndexGets("arena_c"), 0u);
  EXPECT_EQ(IndexGets("beta"), 1u);
  const MountReport boot = Boot();
  EXPECT_TRUE(Mounted(boot, "beta"));
  EXPECT_EQ(TokenOf(boot, "arena_c"), "not_cached");
}

TEST_F(OnDemandTest, AMapWithNoSessionGroupOrAnInvalidIdIsRefusedWithOneRecord) {
  MapRequests requests(env);
  std::string reason;
  EXPECT_FALSE(requests.RequestMap("beta", &reason));
  EXPECT_EQ(reason, "no_session_group");
  const Record* refusal = env.FindRecord("map_request", "outcome", "refused");
  ASSERT_NE(refusal, nullptr);
  EXPECT_EQ(refusal->Get("reason"), "no_session_group");
  ASSERT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;
  EXPECT_FALSE(requests.RequestMap("Not A Map Id", &reason));
  EXPECT_EQ(reason, "bad_id");
}

TEST_F(OnDemandTest, AnUnlistedRequestedMapFailsWithItsConsequence) {
  channel.Publish("arena_c", "1.0.0", MakeSet(4, 'c'));
  channel.Sign(1);
  MapRequests requests(env);
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;
  ASSERT_TRUE(requests.RequestMap("ghost", &reason)) << reason;
  Serve(requests);
  EXPECT_EQ(IndexGets("arena_c"), 0u);
  const Record* answer = env.FindRecord("map_request", "reason", "not_listed");
  ASSERT_NE(answer, nullptr);
  EXPECT_NE(answer->Get("consequence").find("not in your lobby group's map list"), std::string::npos);
}

TEST_F(OnDemandTest, AQueuedRequestWakesTheWorkerAndAStoppedEntryRefuses) {
  MapRequests requests(env);
  int wakes = 0;
  requests.SetWake([&wakes] { ++wakes; });
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;
  EXPECT_EQ(wakes, 1);
  ASSERT_TRUE(requests.RequestMap("beta", &reason)) << reason;
  EXPECT_EQ(wakes, 2);
  requests.Stop();
  EXPECT_FALSE(requests.RequestGroup(kGroup, &reason));
  EXPECT_EQ(reason, "stopped");
  EXPECT_EQ(wakes, 2);
}

// ---- The worker's deferred pass (core/map_channel.h): a request the gap refuses is run when the gap ends, never stranded. ------------
// The worker is built with a short gap in real time, so a test waits for it; the clock is the test's own, set by the test.

constexpr std::uint64_t kDeferGapMs = 100;           // the gap these tests use (real milliseconds)
constexpr std::uint64_t kInsideGapMs = 50;           // a clock time inside that gap
constexpr std::uint64_t kQuietGaps = 2;              // how many gaps a test watches for a pass that must not come
constexpr int kGatedPass = 2;                        // the pass a test holds open while a request arrives
constexpr std::chrono::milliseconds kWaitMs{5000};   // how long a test waits for a pass that must come (a red run waits this long)

// Counts what the worker did and lets a test wait for a count without polling.
class Tally {
 public:
  void Add() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++count_;
    }
    cv_.notify_all();
  }
  bool WaitFor(int n) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, kWaitMs, [this, n] { return count_ >= n; });
  }
  int Value() {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  int count_ = 0;
};

// Stops the worker when the test ends, even when an expectation failed: a worker destroyed while its thread runs terminates.
struct StopOnExit {
  nevr_map_channel::FetchWorker& worker;
  ~StopOnExit() { worker.Stop(kWaitMs); }
};

// A session's request refused inside the worker's gap is run when the gap ends, with no later request. The wake is the PC glue's:
// it ignores the worker's answer, so a refused wake alone must not strand the queued request.
TEST_F(OnDemandTest, AQueuedRequestRefusedInsideTheWorkersGapRunsWhenTheGapEnds) {
  std::atomic<std::uint64_t> now{0};
  Tally passes;
  Tally groups_run;
  MapRequests requests(env);
  nevr_map_channel::FetchWorker worker(
      [&requests, &passes, &groups_run](const std::function<bool()>& keep_going) {
        while (keep_going() && requests.RunNext([&groups_run](const std::string&, const FetchScope&) {
          groups_run.Add();
          return FetchReport();
        })) {
        }
        passes.Add();
      },
      [&now] { return now.load(); }, kDeferGapMs);
  StopOnExit stop{worker};
  requests.SetWake([&worker] { worker.Request(); });
  worker.Start();
  std::string reason;
  EXPECT_TRUE(requests.RequestGroup(kGroup, &reason)) << reason;  // accepted at clock 0: one pass
  EXPECT_TRUE(passes.WaitFor(2));                                  // the start's pass and that one
  now.store(kInsideGapMs);
  EXPECT_TRUE(requests.RequestMap("beta", &reason)) << reason;  // refused inside the gap; the wake's answer is ignored
  std::this_thread::sleep_for(std::chrono::milliseconds(kQuietGaps * kDeferGapMs));  // the worker is parked on the rest of the gap
  now.store(kDeferGapMs);                                                             // the gap ends with no further request
  EXPECT_TRUE(passes.WaitFor(3));  // the deferred pass runs the queued map
  EXPECT_EQ(groups_run.Value(), 2);
  now.store(kDeferGapMs + kInsideGapMs);
  EXPECT_FALSE(worker.Request());  // the deferred pass started the gap: a request inside it is refused, and it too gets a pass
  now.store(kQuietGaps * kDeferGapMs);  // that gap ends
  EXPECT_TRUE(passes.WaitFor(4));
  now.store(kQuietGaps * kDeferGapMs + kQuietGaps * kDeferGapMs);  // the clock moves on with no request of any kind
  std::this_thread::sleep_for(std::chrono::milliseconds(kQuietGaps * kDeferGapMs));
  EXPECT_EQ(passes.Value(), 4);  // and the worker starts no pass of its own
}

// A request made while a pass runs, inside the gap, is run when the gap ends, after that pass.
TEST(WorkerDeferredPass, ARequestMadeDuringAPassInsideTheGapRunsWhenTheGapEnds) {
  std::atomic<std::uint64_t> now{0};
  Tally entered;
  Tally passes;
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool released = false;  // under gate_mutex
  int pass_no = 0;        // the worker's thread only
  nevr_map_channel::FetchWorker worker(
      [&](const std::function<bool()>&) {
        ++pass_no;
        if (pass_no == kGatedPass) {
          entered.Add();
          std::unique_lock<std::mutex> lock(gate_mutex);
          gate_cv.wait(lock, [&released] { return released; });
        }
        passes.Add();
      },
      [&now] { return now.load(); }, kDeferGapMs);
  StopOnExit stop{worker};
  worker.Start();
  EXPECT_TRUE(passes.WaitFor(1));  // the start's pass
  EXPECT_TRUE(worker.Request());   // accepted at clock 0: pass 2, which the gate holds
  EXPECT_TRUE(entered.WaitFor(1));
  now.store(kInsideGapMs);
  EXPECT_FALSE(worker.Request());  // during that pass, inside the gap: refused
  now.store(kDeferGapMs);          // the gap ends while the pass still runs
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    released = true;
  }
  gate_cv.notify_all();
  EXPECT_TRUE(passes.WaitFor(3));  // pass 2 ends, then the deferred pass runs
}

// An accepted request covers the refused one queued before it, and the worker then starts no pass of its own.
TEST(WorkerDeferredPass, AnAcceptedRequestCoversARefusedOneAndStartsNoPassOfItsOwn) {
  std::atomic<std::uint64_t> now{0};
  Tally passes;
  nevr_map_channel::FetchWorker worker([&passes](const std::function<bool()>&) { passes.Add(); },
                                       [&now] { return now.load(); }, kDeferGapMs);
  StopOnExit stop{worker};
  worker.Start();
  EXPECT_TRUE(passes.WaitFor(1));
  EXPECT_TRUE(worker.Request());  // accepted at clock 0
  EXPECT_TRUE(passes.WaitFor(2));
  now.store(kInsideGapMs);
  EXPECT_FALSE(worker.Request());  // refused: a pass is owed
  now.store(kDeferGapMs);
  EXPECT_TRUE(worker.Request());  // accepted: one pass covers the refused request too
  EXPECT_TRUE(passes.WaitFor(3));
  now.store(kDeferGapMs + kQuietGaps * kDeferGapMs);  // the clock moves on with no request: a stale pending flag would run a pass now
  std::this_thread::sleep_for(std::chrono::milliseconds(kQuietGaps * kDeferGapMs));
  EXPECT_EQ(passes.Value(), 3);
}

// The session pin's view on the worker and at boot (core/map_request_serve.h; K1b): a served request is fetched, then listed, then
// its listing is published; a boot publishes the view of its report; nothing is published when nothing was served.
constexpr const char* kServeGroup = "0b6e4c1a-7f3d-4e2a-9c5b-8d1e2f3a4b5c";
constexpr std::uint64_t kServeLevel = 7;             // a listed map's level: a fixture value, the view only carries it
constexpr std::size_t kLevelNamesPerMap = 2;         // the session and the low-spec level names of one listed map (ViewFromMount)
constexpr std::size_t kTwoGroups = 2;                // the groups queued in one pass

TEST(ServeSessionView, AServedGroupIsFetchedThenListedAndItsListingIsPublished) {
  cdn_test::FakeEnv env;
  MapRequests requests(env);
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kServeGroup, &reason)) << reason;
  std::vector<std::string> trace;
  std::vector<nevr_map_session_pin::ChannelView> published;
  nevr_map_channel::ServeSessionRequests(
      requests,
      [&trace](const std::string& group, const FetchScope&) {
        trace.push_back("fetch:" + group);
        return FetchReport{};
      },
      [&trace](const std::string& group) {
        trace.push_back("list:" + group);
        MountReport listing;
        listing.listed.emplace_back(kServeLevel, "arena_c");
        return listing;
      },
      [&published](nevr_map_session_pin::ChannelView view) { published.push_back(std::move(view)); }, [] { return true; },
      [](const std::string&, const std::vector<std::string>&) { return MountReport{}; });
  EXPECT_EQ(trace, (std::vector<std::string>{std::string("fetch:") + kServeGroup, std::string("list:") + kServeGroup}));
  ASSERT_EQ(published.size(), 1u);
  ASSERT_EQ(published[0].listed.size(), kLevelNamesPerMap);
  EXPECT_EQ(published[0].listed[0].second, "arena_c");
}

namespace {

// A client in a session on a map it lacks, with a level load the platform can hold (core/map_session_pin.h, the join hold).
class HoldingGame : public nevr_map_session_pin::GameAccess {
 public:
  static constexpr std::uint64_t kLobby = 0x7000;
  static constexpr std::uint64_t kPin = 0x1234;
  std::vector<nevr_map_session_pin::PinEvent> events;
  bool Callable() override { return true; }
  bool IsServer() override { return false; }
  std::uint64_t Lobby() override { return kLobby; }
  std::int32_t NetState() override { return 5; }
  std::uint64_t OwnerSymbol(std::uint64_t, const char* key) override {
    return std::string(key) == nevr_map_session_pin::kOwnerLevelKey ? nevr_map_session_pin::SessionLevelOf("arena_c") : kPin;
  }
  void SetOwnerSymbol(std::uint64_t, const char*, std::uint64_t) override {}
  std::uint64_t MainLevel() override { return 0; }
  bool WriteNotice(const char*, const char*) override { return true; }
  bool ScheduleLeave() override { return true; }
  std::uint64_t NowMs() override { return 1; }
  void Event(const nevr_map_session_pin::PinEvent& event) override { events.push_back(event); }
  bool CanHoldLoad() override { return true; }
};

}  // namespace

// A join held for a named map hears of a failed fetch of it from the worker's pass, and leaves at once with the fetch's reason.
TEST(ServeSessionView, AFailedFetchOfTheNamedMapEndsTheHeldJoin) {
  namespace sp = nevr_map_session_pin;
  sp::ResetForTest();
  sp::ChannelView view;
  view.listed = {{sp::SessionLevelOf("arena_c"), "arena_c"}};
  view.not_mounted = {{"arena_c", "not_cached"}};
  sp::SetChannelView(view);
  cdn_test::FakeEnv env;
  MapRequests requests(env);
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kServeGroup, &reason)) << reason;
  ASSERT_TRUE(requests.RequestMap("arena_c", &reason)) << reason;
  HoldingGame game;
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  while (requests.RunNext([](const std::string& group, const FetchScope&) {
    FetchReport failed;
    failed.group = group;
    failed.outcome = FetchOutcome::kFailed;
    failed.reason = "http_failed";
    return failed;
  })) {
  }
  sp::TickWith(game);
  const nevr_map_session_pin::PinEvent* skew = nullptr;
  for (const auto& e : game.events)
    if (e.kind == sp::PinEventKind::kSkew) skew = &e;
  ASSERT_NE(skew, nullptr);
  EXPECT_EQ(skew->reason, "fetch_failed");
  sp::ResetForTest();
}

TEST(ServeSessionView, NothingWaitingPublishesNothing) {
  cdn_test::FakeEnv env;
  MapRequests requests(env);
  int published = 0;
  nevr_map_channel::ServeSessionRequests(
      requests, [](const std::string&, const FetchScope&) { return FetchReport{}; },
      [](const std::string&) { return MountReport{}; }, [&published](nevr_map_session_pin::ChannelView) { ++published; },
      [] { return true; }, [](const std::string&, const std::vector<std::string>&) { return MountReport{}; });
  EXPECT_EQ(published, 0);
}

TEST(ServeSessionView, AStoppingPassServesNothingAndPublishesNothing) {
  cdn_test::FakeEnv env;
  MapRequests requests(env);
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kServeGroup, &reason)) << reason;
  int published = 0;
  nevr_map_channel::ServeSessionRequests(
      requests, [](const std::string&, const FetchScope&) { return FetchReport{}; },
      [](const std::string&) { return MountReport{}; }, [&published](nevr_map_session_pin::ChannelView) { ++published; },
      [] { return false; }, [](const std::string&, const std::vector<std::string>&) { return MountReport{}; });
  EXPECT_EQ(published, 0);
  EXPECT_EQ(requests.Waiting(), 1u);  // still waiting for a pass that runs
}

TEST(ServeSessionView, ABootPublishesTheViewOfItsReport) {
  MountReport report;
  report.listed.emplace_back(kServeLevel, "arena_c");
  report.not_mounted.emplace_back("beta", "bad_hash");
  std::vector<nevr_map_session_pin::ChannelView> published;
  nevr_map_channel::PublishBootView(report, [&published](nevr_map_session_pin::ChannelView view) {
    published.push_back(std::move(view));
  });
  ASSERT_EQ(published.size(), 1u);
  EXPECT_EQ(published[0].listed.size(), kLevelNamesPerMap);
  EXPECT_EQ(published[0].not_mounted, report.not_mounted);
}

// Every waiting group is served in the one pass, each fetched and then listed before the next: the loop does not stop after one.
TEST(ServeSessionView, EveryWaitingGroupIsServedAndListedInThePass) {
  constexpr const char* kOtherGroup = "5f2a9d10-3b8c-4e7f-a1d2-6c9e0b4f7a31";
  cdn_test::FakeEnv env;
  MapRequests requests(env);
  std::string reason;
  ASSERT_TRUE(requests.RequestGroup(kServeGroup, &reason)) << reason;
  ASSERT_TRUE(requests.RequestGroup(kOtherGroup, &reason)) << reason;
  ASSERT_EQ(requests.Waiting(), kTwoGroups);
  std::vector<std::string> trace;
  std::size_t published = 0;
  nevr_map_channel::ServeSessionRequests(
      requests,
      [&trace](const std::string& group, const FetchScope&) {
        trace.push_back("fetch:" + group);
        return FetchReport{};
      },
      [&trace](const std::string& group) {
        trace.push_back("list:" + group);
        return MountReport{};
      },
      [&published](nevr_map_session_pin::ChannelView) { ++published; }, [] { return true; }, [](const std::string&, const std::vector<std::string>&) { return MountReport{}; });
  EXPECT_EQ(trace, (std::vector<std::string>{std::string("fetch:") + kServeGroup, std::string("list:") + kServeGroup,
                                              std::string("fetch:") + kOtherGroup, std::string("list:") + kOtherGroup}));
  EXPECT_EQ(published, kTwoGroups);
  EXPECT_EQ(requests.Waiting(), 0u);
}

}  // namespace
