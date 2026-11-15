// The owner's version rules end to end on the Test edge (quest/tests/cdn_fake_env.h, map_fake_fs.h, a scripted server and a
// stopped clock): the request entry (core/map_request.h), the channel's worker with its hot mount (Channel::ServeRequests), and
// the session pin's decision (core/map_session_pin.h) over the view the run's mounts give. A "server pin" is the pin a server
// running that version publishes; "loads" means the pin's decision is none, so the client stays and the game loads the level.
#include "core/map_request.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/map_channel.h"
#include "core/map_session_pin.h"
#include "quest/tests/cdn_fake_env.h"
#include "quest/tests/map_fake_fs.h"
#include "quest/tests/map_run_fixture.h"

namespace {

using namespace map_run_fixture;
namespace mc = nevr_map_channel;
namespace sp = nevr_map_session_pin;

std::vector<std::string> g_pin_requests;
void RecordPinRequest(const std::string& id) { g_pin_requests.push_back(id); }

class MapRequestTest : public ::testing::Test, public Fixture {
 protected:
  void SetUp() override {
    sp::ResetForTest();
    g_pin_requests.clear();
  }
  void TearDown() override { sp::ResetForTest(); }

  // A client process: the channel booted from whatever the cache holds, its view handed to the session pin.
  void BootClient() {
    mc::Settings settings;
    settings.base_url = kBase;
    settings.keys = {key.pin};
    ChannelConfig platform;
    platform.dir = kDir;
    client = std::make_unique<mc::Channel>(platform, settings);
    requests = std::make_unique<MapRequests>(env, /*is_server=*/false);
    const mc::BootResult& boot = client->Boot(env, fs, mounter, nullptr, kNow, {});
    ASSERT_EQ(boot.start, mc::Start::kRun);
    run = boot.report;
    Publish();
    sp::SetFetchRequest(&RecordPinRequest);
  }

  // The worker answers every waiting request, each followed by its hot mount.
  void Serve() {
    bool done = false;
    client->ServeRequests(
        env, fs, nullptr, *requests, [] { return std::string(kNow); }, [&] { return !done; }, nullptr,
        [&](const FetchReport& f, const MountReport& m, const MountReport& folded) {
          fetched = f;
          mounted = m;
          run = folded;
          if (requests->Waiting() == 0) done = true;
        });
    Publish();
  }

  void Publish() {
    sp::SetChannelView(sp::ViewFromMount(run));
    std::vector<sp::MapPin> pins;
    for (const MountedMap& m : run.staged) pins.push_back({m.id, m.version, m.index_sha256, m.platform, m.source_sha256, m.title, m.level});
    sp::SetMountedPins(pins);
  }

  // The pin a server running the version this run mounted publishes.
  std::uint64_t ServerPinOfMounted(const std::string& id) const {
    for (const MountedMap& m : run.staged) {
      if (m.id == id) return sp::PinValue({m.id, m.version, m.index_sha256, m.platform, m.source_sha256, m.title, m.level});
    }
    ADD_FAILURE() << id << " is not mounted";
    return 0;
  }

  sp::ClientDecision Join(const std::string& id, std::uint64_t server_pin) {
    return sp::DecideClient(sp::SessionLevelOf(id), server_pin);
  }

  std::size_t IndexGets(const std::string& id) { return GetsOf(std::string(kBase) + "blobs/sha256/" + channel.IndexSha(id)); }

  std::unique_ptr<mc::Channel> client;
  std::unique_ptr<MapRequests> requests;
  MountReport run, mounted;
  FetchReport fetched;
  std::string reason;
};

// A server pin no client could hold: some version of the map this client does not have.
constexpr std::uint64_t kOtherServerPin = 0x5eed;

// Owner: "if a player trys to load arena_C but doesnt have it it should download, then mount, and load." Nothing is cached, the
// group lists two maps; only arena_c is fetched, it mounts mid-run, and the same server's session then loads.
TEST(MapRequestConsequence, NoRoomSaysWhatTheMapNeedsWhatCanBeMadeFreeAndTheCap) {
  nevr_maps::FetchReport r;
  r.outcome = nevr_maps::FetchOutcome::kFailed;
  r.reason = "over_budget";
  r.no_room_id = "arena_c";
  r.no_room_needed = std::uint64_t{3} << 30;
  r.no_room_available = std::uint64_t{512} << 20;
  r.no_room_cap = std::uint64_t{20} << 30;
  EXPECT_EQ(nevr_maps::RequestConsequence(r, true, false),
            "not enough room for arena_c: needs 3.0 GiB, 512 MiB can be made free, cap 20.0 GiB: no maps were removed for it, "
            "and an old copy is never loaded in its place");
  r.no_room_cap = 0;
  EXPECT_NE(nevr_maps::RequestConsequence(r, true, false).find("cap none"), std::string::npos);
}

TEST_F(MapRequestTest, ArenaCNotCachedIsDownloadedMountedAndLoadsWithNoRestart) {
  channel.Publish("arena_c", "1.0.0", MakeSet(4, 'c'));
  channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
  channel.Sign(1);
  BootClient();
  ASSERT_TRUE(run.mounted.empty());

  ASSERT_TRUE(requests->RequestGroup(kGroup, &reason)) << reason;  // the lobby session succeeded
  Serve();
  EXPECT_EQ(IndexGets("arena_c"), 0u);  // on demand: the group's manifest only
  EXPECT_EQ(IndexGets("beta"), 0u);

  const sp::ClientDecision before = Join("arena_c", kOtherServerPin);
  EXPECT_EQ(before.action, sp::ClientAction::kLeave);  // never loads a level it does not hold
  EXPECT_TRUE(before.request_fetch);
  EXPECT_EQ(before.notice_line, "Downloading arena_c; join again when it is ready.");
  ASSERT_TRUE(requests->RequestMap(before.id, &reason)) << reason;
  Serve();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFetched);
  EXPECT_EQ(IndexGets("arena_c"), 1u);
  EXPECT_EQ(IndexGets("beta"), 0u);
  EXPECT_TRUE(Mounted(mounted, "arena_c"));
  EXPECT_TRUE(mounter.Called("set map:arena_c"));
  EXPECT_FALSE(mounter.Called("set map:beta"));

  const sp::ClientDecision after = Join("arena_c", ServerPinOfMounted("arena_c"));
  EXPECT_EQ(after.action, sp::ClientAction::kNone);  // loads
  const Record* answer = env.FindRecord("map_request", "id", "arena_c");
  ASSERT_NE(answer, nullptr);
  EXPECT_EQ(answer->Get("outcome"), "fetched");
}

// The server runs V and the cache holds V: no fetch is asked, and the session loads.
TEST_F(MapRequestTest, ServerOnTheCachedVersionLoadsWithNoFetch) {
  channel.Publish("arena_c", "1.0.0", MakeSet(4, 'c'));
  channel.Sign(1);
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  BootClient();
  const std::size_t gets = env.requests.size();
  EXPECT_EQ(Join("arena_c", ServerPinOfMounted("arena_c")).action, sp::ClientAction::kNone);
  EXPECT_TRUE(g_pin_requests.empty());
  EXPECT_EQ(env.requests.size(), gets);
}

// The cache holds V-1 and the stored manifest lists V (the last update did not finish): V-1 is refused at boot, the session
// asks for V, only the changed piece is fetched, V mounts mid-run and a server on V loads.
TEST_F(MapRequestTest, CacheOnTheOldVersionFetchesTheChangedPiecesAndLoadsTheCurrent) {
  const std::string v1 = MakeSet(6, 'c');
  channel.Publish("arena_c", "1.0.0", v1);
  channel.Sign(1);
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  std::string v2 = v1;
  v2[FramesStart(6) + 2 * kFrameBytes + 20] ^= 1;
  channel.Publish("arena_c", "1.1.0", v2);
  channel.Sign(2);
  const std::string v2_index = std::string(kBase) + "blobs/sha256/" + channel.IndexSha("arena_c");
  const auto held_back = env.server.find(v2_index)->second;
  env.server.erase(v2_index);
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFailed);  // the manifest listing 1.1.0 is stored, the version is not
  env.server[v2_index] = held_back;

  BootClient();
  EXPECT_EQ(TokenOf(run, "arena_c"), kStaleToken);
  const sp::ClientDecision before = Join("arena_c", kOtherServerPin);
  EXPECT_EQ(before.action, sp::ClientAction::kLeave);
  EXPECT_TRUE(before.request_fetch);

  ASSERT_TRUE(requests->RequestGroup(kGroup, &reason));
  ASSERT_TRUE(requests->RequestMap("arena_c", &reason));
  Serve();
  EXPECT_EQ(fetched.pieces_fetched, 1u);
  EXPECT_EQ(fetched.pieces_reused, 6u);
  ASSERT_TRUE(Mounted(mounted, "arena_c"));
  ASSERT_EQ(mounted.staged.size(), 1u);
  EXPECT_EQ(mounted.staged[0].version, "1.1.0");
  EXPECT_EQ(Join("arena_c", ServerPinOfMounted("arena_c")).action, sp::ClientAction::kNone);
}

// This run mounted V-1 at boot, and the channel moved to V mid-run: V is stored for the next launch, the mounted set is never
// replaced, and no server is joined on either version this run, so the old version is never played once V is listed.
TEST_F(MapRequestTest, AnUpdateListedMidRunIsStoredAndTheMountedOldVersionIsNeverPlayed) {
  channel.Publish("arena_c", "1.0.0", MakeSet(4, 'c'));
  channel.Sign(1);
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  BootClient();
  const std::uint64_t old_pin = ServerPinOfMounted("arena_c");
  ASSERT_EQ(Join("arena_c", old_pin).action, sp::ClientAction::kNone);

  channel.Publish("arena_c", "1.1.0", MakeSet(4, 'd'));
  channel.Sign(2);
  ASSERT_TRUE(requests->RequestGroup(kGroup, &reason));
  Serve();
  EXPECT_EQ(fetched.maps_updated, 1u);  // a held map at an older version is in the on-demand scope
  EXPECT_EQ(TokenOf(mounted, "arena_c"), kMountedOtherVersionToken);
  const sp::ClientDecision old_server = Join("arena_c", old_pin);
  EXPECT_EQ(old_server.action, sp::ClientAction::kLeave);
  EXPECT_EQ(old_server.reason, "updated");
  EXPECT_EQ(old_server.notice_line, "Map arena_c was updated; restart the game to play it.");
  EXPECT_EQ(Join("arena_c", kOtherServerPin).reason, "updated");

  // The next launch mounts the stored 1.1.0.
  sp::ResetForTest();
  FakeMounter next_mounter{env};
  CacheMount next(env, fs, config, next_mounter, nullptr);
  const MountReport relaunch = next.Mount({kGroup}, kNow);
  ASSERT_EQ(relaunch.staged.size(), 1u);
  EXPECT_EQ(relaunch.staged[0].version, "1.1.0");
}

// The server still runs V-1, which the manifest no longer lists; the client holds V. The client leaves before loading, the
// fetch finds its copy current, and the answer says in plain words that the old version cannot be joined. V-1 is never fetched.
TEST_F(MapRequestTest, AServerOnAVersionNoLongerListedIsRefusedBeforeLoad) {
  channel.Publish("arena_c", "1.0.0", MakeSet(4, 'c'));
  channel.Sign(1);
  const std::string v1_index = std::string(kBase) + "blobs/sha256/" + channel.IndexSha("arena_c");
  channel.Publish("arena_c", "1.1.0", MakeSet(4, 'd'));
  channel.Sign(2);
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  BootClient();
  const sp::MapPin old_server{"arena_c", "1.0.0", "", kPlatformPc, std::string(64, 'e'), "", sp::SessionLevelOf("arena_c")};
  const sp::ClientDecision decision = Join("arena_c", sp::PinValue(old_server));
  EXPECT_EQ(decision.action, sp::ClientAction::kLeave);
  EXPECT_EQ(decision.reason, "version");
  EXPECT_EQ(decision.notice_line, "Server's arena_c is not your 1.1.0; checking for update.");
  ASSERT_TRUE(requests->RequestGroup(kGroup, &reason));
  ASSERT_TRUE(requests->RequestMap("arena_c", &reason));
  Serve();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kCached);
  EXPECT_EQ(GetsOf(v1_index), 0u);
  const Record* answer = env.FindRecord("map_request", "id", "arena_c");
  ASSERT_NE(answer, nullptr);
  EXPECT_NE(answer->Get("consequence").find("cannot be joined until it updates"), std::string::npos) << answer->Get("consequence");
}

// Offline, and the stored manifest lists a version the cache does not hold: the old copy is refused, the request fails, and the
// old version is never registered.
TEST_F(MapRequestTest, OfflineWithAStaleCopyIsRefusedAndTheOldVersionNeverLoads) {
  channel.Publish("arena_c", "1.0.0", MakeSet(4, 'c'));
  channel.Sign(1);
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  channel.Publish("arena_c", "1.1.0", MakeSet(4, 'd'));
  channel.Sign(2);
  env.server.erase(std::string(kBase) + "blobs/sha256/" + channel.IndexSha("arena_c"));
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFailed);
  env.server.clear();  // offline

  BootClient();
  EXPECT_EQ(TokenOf(run, "arena_c"), kStaleToken);
  ASSERT_TRUE(requests->RequestGroup(kGroup, &reason));
  ASSERT_TRUE(requests->RequestMap("arena_c", &reason));
  Serve();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_FALSE(mounter.Called("set map:arena_c"));
  EXPECT_EQ(Join("arena_c", kOtherServerPin).action, sp::ClientAction::kLeave);
  const Record* answer = env.FindRecord("map_request", "id", "arena_c");
  ASSERT_NE(answer, nullptr);
  EXPECT_NE(answer->Get("consequence").find("an old copy is never loaded"), std::string::npos) << answer->Get("consequence");
}

// A map the group's manifest does not list is refused with plain words, and nothing is fetched; a map asked before any session
// group is known is refused before the queue.
TEST_F(MapRequestTest, AMapTheManifestDoesNotListIsRefusedAndNothingIsFetched) {
  channel.Publish("arena_c", "1.0.0", MakeSet(4, 'c'));
  channel.Sign(1);
  BootClient();
  EXPECT_FALSE(requests->RequestMap("phantom", &reason));
  EXPECT_EQ(reason, "no_session_group");
  ASSERT_TRUE(requests->RequestGroup(kGroup, &reason));
  ASSERT_TRUE(requests->RequestMap("ghost", &reason));
  Serve();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(fetched.reason, "not_listed");
  EXPECT_EQ(IndexGets("arena_c"), 0u);
  const Record* answer = env.FindRecord("map_request", "id", "ghost");
  ASSERT_NE(answer, nullptr);
  EXPECT_NE(answer->Get("consequence").find("does not offer this map"), std::string::npos);
  EXPECT_EQ(Join("ghost", kOtherServerPin).action, sp::ClientAction::kNone);  // not listed: the pin has nothing to decide
}

// A server fetches its whole group whatever is asked; a client only what it names.
TEST_F(MapRequestTest, AServerRequestTakesTheWholeGroup) {
  channel.Publish("arena_c", "1.0.0", MakeSet(4, 'c'));
  channel.Publish("beta", "1.0.0", MakeSet(4, 'b'));
  channel.Sign(1);
  MapRequests server(env, /*is_server=*/true);
  ASSERT_TRUE(server.RequestGroup(kGroup, &reason));
  EXPECT_EQ(server.SessionGroup(), "");  // a server has no session group for the pin to ask
  ASSERT_TRUE(server.RunNext([&](const std::string& group, const FetchScope& scope) {
    EXPECT_FALSE(scope.on_demand);
    return FetchGroup(env, fs, config, group, {}, nullptr, kNow, keep_going, nullptr, scope);
  }));
  EXPECT_EQ(IndexGets("arena_c"), 1u);
  EXPECT_EQ(IndexGets("beta"), 1u);
}

}  // namespace
