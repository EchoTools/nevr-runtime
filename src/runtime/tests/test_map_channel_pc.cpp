// The PC channel's mount glue (runtime/content/map_channel.cpp; docs/adr/0048 decisions 5, 9, 13 and 17): the mount observer
// (OnMount), a client's hot mount (ServedRequest) and a server's mid-run group (FetchServerGroup), compiled from the production
// file without its Win32 boot (NEVR_MAP_CHANNEL_TESTING). The core runs for real on the Test edge (quest/tests/map_run_fixture.h:
// a fake Env and MapFs, a scripted server with signed manifests). The registry, the script provider, the config-alias hook and the
// server's join hold are recorders here. The registry keeps its contract (core/resource_set_registry.h): a set registered after
// a mount is mounted by the first frame after MountLaterSets, and the observer runs inside that mount, with the sets it served in
// ReportJson and the ones it refused in the report.
#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include "runtime/content/map_channel.cpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/tests/map_run_fixture.h"

namespace {

namespace pc = nevr_map_channel::pc;
namespace sp = nevr_map_session_pin;
namespace fx = map_run_fixture;

struct Registry {
  std::vector<std::string> pending;  // registered since the last mount
  std::vector<std::string> mounted;  // served, in ReportJson
  std::set<std::string> refuse;      // what the next mount refuses (a key the stock or an earlier mount holds)
  bool mount_asked = false;
};
Registry g_registry;

std::vector<std::string> g_script_withdrawals;
std::vector<std::string> g_fetch_asks;

constexpr const char* kArena = "arena_c";
constexpr const char* kArenaOwner = "map:arena_c";

std::uint64_t ArenaLevel() { return sp::SessionLevelOf(kArena); }

// A server join release, with the lobby pin the release found published.
struct Release {
  std::string group;
  std::uint64_t pin = 0;
};
std::vector<Release> g_releases;

bool Has(const std::vector<std::string>& list, const std::string& owner) {
  return std::find(list.begin(), list.end(), owner) != list.end();
}

}  // namespace

namespace nevr_resource_sets {

Outcome RegisterFileResourceSet(const MapFileRequest& request, std::int64_t) {
  g_registry.pending.push_back(request.owner);
  return Outcome{};
}

void WithdrawFileResourceSet(const std::string& owner) {
  for (std::vector<std::string>* list : {&g_registry.pending, &g_registry.mounted})
    list->erase(std::remove(list->begin(), list->end(), owner), list->end());
}

void MountLaterSets() { g_registry.mount_asked = true; }

bool AwaitsLaterMount(const std::string& owner) { return Has(g_registry.pending, owner); }

bool HoldsKey(std::uint64_t, std::uint64_t) { return false; }

std::string ReportJson() {
  nlohmann::json doc = nlohmann::json::array();
  for (const std::string& owner : g_registry.mounted) doc.push_back({{"plugin", owner}, {"levels", nlohmann::json::array()}});
  return doc.dump();
}

const char* RefusalName(Refusal) { return "stock_key"; }

}  // namespace nevr_resource_sets

namespace nevr_map_scripts {

Outcome Register(const std::string&, std::uint64_t, std::int64_t, const std::string&, bool, std::uint64_t, bool) { return {}; }
Outcome RegisterRekey(const std::string&, std::uint64_t, std::uint64_t, const std::string&, bool) { return {}; }
Outcome RegisterDerived(const std::string&, std::uint64_t, std::int64_t, const std::string&, const std::string&, bool) {
  return {};
}
std::size_t Withdraw(const std::string& owner) {
  g_script_withdrawals.push_back(owner);
  return 0;
}
bool InstallMapScriptsRewriter() { return true; }

}  // namespace nevr_map_scripts

namespace nevr_config_alias {
bool InstallIfStaged() { return false; }
}  // namespace nevr_config_alias

namespace nevr_map_hold {
void NoteFetchProgress(const nevr_maps::FetchProgress&) {}
void NoteFetchFailed(const std::string& group, const std::string& reason) { ADD_FAILURE() << group << " failed: " << reason; }
void NoteGroupMounted(const std::string& group, const nevr_maps::MountReport&) {
  g_releases.push_back({group, sp::ServerPinFor(ArenaLevel())});
}
}  // namespace nevr_map_hold

namespace {

void RecordFetchAsk(const std::string& id) { g_fetch_asks.push_back(id); }

// A client that can hold its level load, in a lobby whose session is on arena_c.
class ClientGame : public sp::GameAccess {
 public:
  std::map<std::string, std::uint64_t> owner;
  std::uint64_t now = 1000;  // never advanced: a release is never the hold's ceiling
  std::vector<sp::PinEvent> events;

  bool Callable() override { return true; }
  bool IsServer() override { return false; }
  std::uint64_t Lobby() override { return 0x10b; }
  std::int32_t NetState() override { return 7; }
  std::uint64_t OwnerSymbol(std::uint64_t, const char* key) override {
    const auto it = owner.find(key);
    return it == owner.end() ? 0 : it->second;
  }
  void SetOwnerSymbol(std::uint64_t, const char*, std::uint64_t) override {}
  std::uint64_t MainLevel() override { return 0; }
  bool WriteNotice(const char*, const char*) override { return true; }
  bool ScheduleLeave() override { return true; }
  std::uint64_t NowMs() override { return now; }
  void Event(const sp::PinEvent& event) override { events.push_back(event); }
  bool CanHoldLoad() override { return true; }

  std::size_t Count(sp::PinEventKind kind) const {
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(), [kind](const sp::PinEvent& e) { return e.kind == kind; }));
  }
  const sp::PinEvent* First(sp::PinEventKind kind) const {
    for (const sp::PinEvent& e : events)
      if (e.kind == kind) return &e;
    return nullptr;
  }
  bool Held() const { return sp::JoinHeldFor(ArenaLevel(), now); }
};

constexpr const char* kServerDir = "C:/nevr/server/maps";
constexpr const char* kClientDir = "C:/nevr/client/maps";

class MapChannelGlue : public ::testing::Test, public fx::Fixture {
 protected:
  void SetUp() override {
    Reset();
    Publish("1.0.0", 'c', 1);
  }

  void TearDown() override {
    Reset();
    for (const std::unique_ptr<pc::State>& st : processes_) {
      st->env.release();
      st->fs.release();
    }
  }

  void Publish(const char* version, char fill, int sequence) {
    channel.Publish(kArena, version, fx::MakeSet(4, fill));
    channel.Sign(sequence);
  }

  // A new process: what one game process holds in memory is forgotten.
  static void Reset() {
    sp::ResetForTest();
    g_registry = Registry();
    g_script_withdrawals.clear();
    g_fetch_asks.clear();
    g_releases.clear();
    pc::g_state = nullptr;
  }

  // One game process, left as BootUnguarded leaves it: the channel booted from its own cache folder, its view published and the
  // boot mount run. The Env and MapFs are the fixture's, borrowed (TearDown releases them).
  pc::State* StartProcess(bool server, const char* dir) {
    auto st = std::make_unique<pc::State>();
    st->server = server;
    st->env.reset(&env);
    st->fs.reset(&fs);
    env.MakeDirs(dir);
    nevr_map_channel::Settings settings;
    settings.base_url = fx::kBase;
    settings.keys = {key.pin};
    nevr_maps::ChannelConfig platform;
    platform.dir = dir;
    st->channel = std::make_unique<nevr_map_channel::Channel>(platform, settings);
    st->mounter = std::make_unique<pc::PcMounter>();
    pc::g_state = st.get();
    const nevr_map_channel::BootResult& boot = st->channel->Boot(env, fs, *st->mounter, nullptr, fx::kNow, {});
    EXPECT_EQ(boot.start, nevr_map_channel::Start::kRun);
    st->deferred = boot.report.deferred;
    pc::GrantMounted(st.get(), boot.report);
    st->run = boot.report;
    sp::SetChannelView(sp::ViewFromMount(boot.report));
    if (!server) st->requests = std::make_unique<nevr_maps::MapRequests>(env, /*is_server=*/false);
    Mount();
    processes_.push_back(std::move(st));
    return processes_.back().get();
  }

  // The registry's mount: it serves what is registered (or refuses it) and runs the observer inside, before the frame ends.
  static void Mount() {
    nevr_resource_sets::MountReport report;
    for (const std::string& owner : g_registry.pending) {
      if (g_registry.refuse.count(owner) != 0) {
        nevr_resource_sets::MountReport::Refused refused;
        refused.plugin = owner;
        refused.outcome.refusal = nevr_resource_sets::Refusal::kStockKey;
        report.refusals.push_back(refused);
        ++report.refused;
      } else {
        g_registry.mounted.push_back(owner);
        ++report.sets;
      }
    }
    g_registry.pending.clear();
    pc::OnMount(report);
  }

  // The game's frame (resource_sets.cpp OnFrame): the later mount runs only when one was asked for.
  static void Frame() {
    if (!g_registry.mount_asked) return;
    g_registry.mount_asked = false;
    Mount();
  }

  // The client's worker answers its waiting requests; each answer's hot mount is the production glue.
  void Serve(pc::State* st) {
    bool done = false;
    st->channel->ServeRequests(
        env, fs, nullptr, *st->requests, [] { return std::string(fx::kNow); }, [&] { return !done; }, nullptr,
        [&](const nevr_maps::FetchReport&, const nevr_maps::MountReport& mounted, const nevr_maps::MountReport& run) {
          pc::ServedRequest(st, mounted, run);
          if (st->requests->Waiting() == 0) done = true;
        });
  }

  // A lobby session succeeded: the client learns its group's manifest, and with it the maps it lacks.
  void SessionSucceeded(pc::State* st) {
    std::string reason;
    ASSERT_TRUE(st->requests->RequestGroup(fx::kGroup, &reason)) << reason;
    Serve(st);
    Frame();
  }

  // A dedicated server's first session create on the group: its worker fetches the group mid-run, and the frame mounts it.
  pc::State* ServerFetchesTheGroup() {
    pc::State* server = StartProcess(true, kServerDir);
    EXPECT_EQ(pc::FetchServerGroup(server, fx::kGroup, keep_going).reason, "");
    Frame();
    return server;
  }

  // A client without arena_c joins the server's session: the join is held, the map is asked for, and the worker downloads it.
  pc::State* ClientDownloadsArenaWhileHeld(ClientGame& game, std::uint64_t server_pin) {
    pc::State* client = StartProcess(false, kClientDir);
    sp::SetFetchRequest(&RecordFetchAsk);
    SessionSucceeded(client);
    game.owner[sp::kOwnerLevelKey] = ArenaLevel();
    game.owner[sp::kOwnerPinKey] = server_pin;
    sp::TickWith(game);
    EXPECT_EQ(game.Count(sp::PinEventKind::kHoldArmed), 1u);
    EXPECT_EQ(g_fetch_asks, std::vector<std::string>{kArena});
    std::string reason;
    EXPECT_TRUE(client->requests->RequestMap(kArena, &reason)) << reason;
    Serve(client);
    EXPECT_TRUE(nevr_resource_sets::AwaitsLaterMount(kArenaOwner));
    EXPECT_TRUE(g_registry.mount_asked) << "the hot mount asks the registry for a later mount";
    return client;
  }

  std::vector<std::unique_ptr<pc::State>> processes_;
};

// Owner: "if a player trys to load arena_C but doesnt have it it should download, then mount, and load". The hot mount asks for
// the registry's later mount, that mount's observer publishes the session's pin, and the next tick releases the same join.
TEST_F(MapChannelGlue, AClientThatLacksTheMapDownloadsItAndTheLaterMountReleasesTheHeldJoin) {
  ServerFetchesTheGroup();
  const std::uint64_t server_pin = sp::ServerPinFor(ArenaLevel());
  ASSERT_NE(server_pin, 0u);
  Reset();
  ClientGame game;
  ClientDownloadsArenaWhileHeld(game, server_pin);
  sp::TickWith(game);
  EXPECT_TRUE(game.Held()) << "registered is not mounted";

  // The observer of a mount that was running while the worker registered the map: the map waits for the next mount.
  pc::OnMount(nevr_resource_sets::MountReport{});
  EXPECT_TRUE(g_script_withdrawals.empty()) << "a map about to be served kept its scripts";
  EXPECT_EQ(sp::ServerPinFor(ArenaLevel()), 0u);
  sp::TickWith(game);
  EXPECT_TRUE(game.Held());

  Frame();
  EXPECT_TRUE(g_script_withdrawals.empty());
  EXPECT_EQ(sp::ServerPinFor(ArenaLevel()), server_pin) << "the later mount's observer published the session's pin";
  sp::TickWith(game);
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldReleased), 1u);
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldLeave), 0u);
  EXPECT_FALSE(game.Held());
}

// A later mount that refuses the map never releases the hold: no pin is published, and the next tick leaves with a plain notice.
TEST_F(MapChannelGlue, ALaterMountThatRefusesTheMapNeverReleasesTheHeldJoin) {
  ServerFetchesTheGroup();
  const std::uint64_t server_pin = sp::ServerPinFor(ArenaLevel());
  ASSERT_NE(server_pin, 0u);
  Reset();
  ClientGame game;
  ClientDownloadsArenaWhileHeld(game, server_pin);
  g_registry.refuse.insert(kArenaOwner);
  Frame();
  EXPECT_EQ(sp::ServerPinFor(ArenaLevel()), 0u);
  sp::TickWith(game);
  const sp::PinEvent* leave = game.First(sp::PinEventKind::kHoldLeave);
  ASSERT_NE(leave, nullptr);
  EXPECT_EQ(leave->reason, "mount_refused");
  EXPECT_EQ(leave->notice, "Map arena_c could not be loaded this run. See the nevr log.");
  EXPECT_FALSE(game.Held());
  for (int i = 0; i < 3; ++i) sp::TickWith(game);
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldReleased), 0u);
}

// Owner: "if a server has arena c loaded and arena c gets updated clients cant join the old arena c version". A group the server
// fetches mid-run is mounted by the registry's later mount, and that mount's observer releases the group's joins only after it
// published the lobby pin. A client still on the old version of the map is refused that pin.
TEST_F(MapChannelGlue, AServerGroupFetchedMidRunIsMountedAndPinnedBeforeItsJoinsRelease) {
  pc::State* client = StartProcess(false, kClientDir);  // a client that played arena_c 1.0.0 in an earlier run
  SessionSucceeded(client);
  std::string reason;
  ASSERT_TRUE(client->requests->RequestMap(kArena, &reason)) << reason;
  Serve(client);
  Frame();

  Publish("1.1.0", 'd', 2);
  Reset();
  pc::State* server = StartProcess(true, kServerDir);
  ASSERT_TRUE(server->run.staged.empty());
  ASSERT_EQ(pc::FetchServerGroup(server, fx::kGroup, keep_going).reason, "");
  EXPECT_TRUE(g_releases.empty()) << "no join is released before the registry mounts the group";
  EXPECT_TRUE(nevr_resource_sets::AwaitsLaterMount(kArenaOwner));
  Frame();
  ASSERT_EQ(g_releases.size(), 1u) << "without the later mount the joins wait for the hold's ceiling";
  EXPECT_EQ(g_releases[0].group, fx::kGroup);
  const std::uint64_t server_pin = sp::ServerPinFor(ArenaLevel());
  ASSERT_NE(server_pin, 0u) << "an unpinned lobby lets every client in on whatever it holds";
  EXPECT_EQ(g_releases[0].pin, server_pin) << "released only once the lobby pin is published";
  ASSERT_EQ(server->run.staged.size(), 1u);
  EXPECT_EQ(server->run.staged[0].version, "1.1.0");
  EXPECT_TRUE(g_script_withdrawals.empty());

  Reset();
  client = StartProcess(false, kClientDir);  // the old client's next start mounts the copy it holds
  ASSERT_EQ(client->run.staged.size(), 1u);
  EXPECT_EQ(client->run.staged[0].version, "1.0.0");
  ASSERT_NE(sp::ServerPinFor(ArenaLevel()), 0u);
  EXPECT_NE(sp::ServerPinFor(ArenaLevel()), server_pin);
  EXPECT_EQ(sp::DecideClient(ArenaLevel(), server_pin).action, sp::ClientAction::kLeave);
}

}  // namespace
