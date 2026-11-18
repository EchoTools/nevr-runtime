// The session map pin's pure part (core/map_session_pin.h, ADR 0048 decision 9): the pin value, both level names, the view and
// its replacement, the decision table, the notice line, the login and registration entries, and the tick over a fake game.
// The vectors that pin the level names and the pin value are the ones the PC and the Quest were tested against.

#include "core/map_session_pin.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/cargo_session_pin.h"
#include "core/map_server_hold.h"
#include "nevr/symbol_hash.h"

namespace {

namespace sp = nevr_map_session_pin;

const std::string kSourceA = "0123456789abcdef" + std::string(48, 'a');
const std::string kSourceB = "fedcba9876543210" + std::string(48, 'b');

sp::MapPin Pin(const std::string& id, const std::string& version, const std::string& source,
               const std::string& platform = "windows-x86_64") {
  sp::MapPin pin;
  pin.id = id;
  pin.version = version;
  pin.source_sha256 = source;
  pin.index_sha256 = std::string(64, platform[0] == 'w' ? '1' : '2');
  pin.platform = platform;
  pin.title = id;
  pin.level = EchoVR::CSymbol64Hash(id.c_str());
  return pin;
}

std::uint64_t Level(const std::string& id) { return EchoVR::CSymbol64Hash(id.c_str()); }

constexpr std::uint64_t kStockLevel = 0x43e2da7914642604ull;  // mpl_combat_dyson
constexpr std::uint64_t kLobby = 0x7000;
constexpr std::uint64_t kPinOfNothing = 0x55;

// A game that counts every call the tick makes. Each platform's half is one implementation of this interface.
class FakeGame : public sp::GameAccess {
 public:
  bool callable = true;
  bool server = false;
  std::uint64_t lobby = kLobby;
  std::int32_t state = 9;
  std::map<std::string, std::uint64_t> owner;
  std::uint64_t main_level = 0;
  bool schedule_ok = true;
  bool notice_ok = true;
  bool can_hold = false;   // the platform can hold its level load (CanHoldLoad)
  bool replay_ok = true;   // ReplayLoad's answer
  int replays = 0;
  std::uint64_t now = 1000;
  int game_calls = 0;  // every call the tick makes into the game, except Callable
  int set_calls = 0;
  int schedule_calls = 0;
  std::vector<std::pair<std::string, std::string>> notices;
  std::vector<sp::PinEvent> events;

  bool Callable() override { return callable; }
  bool IsServer() override {
    ++game_calls;
    return server;
  }
  std::uint64_t Lobby() override {
    ++game_calls;
    return lobby;
  }
  std::int32_t NetState() override {
    ++game_calls;
    return state;
  }
  std::uint64_t OwnerSymbol(std::uint64_t, const char* key) override {
    ++game_calls;
    auto it = owner.find(key);
    return it == owner.end() ? 0 : it->second;
  }
  void SetOwnerSymbol(std::uint64_t, const char* key, std::uint64_t value) override {
    ++game_calls;
    ++set_calls;
    owner[key] = value;
  }
  std::uint64_t MainLevel() override {
    ++game_calls;
    return main_level;
  }
  bool WriteNotice(const char* title, const char* line) override {
    ++game_calls;
    notices.emplace_back(title, line);
    return notice_ok;
  }
  bool ScheduleLeave() override {
    ++game_calls;
    ++schedule_calls;
    return schedule_ok;
  }
  std::uint64_t NowMs() override { return now; }
  void Event(const sp::PinEvent& event) override { events.push_back(event); }
  bool CanHoldLoad() override { return can_hold; }
  bool ReplayLoad() override {
    ++replays;
    return replay_ok;
  }

  std::size_t Count(sp::PinEventKind kind) const {
    std::size_t n = 0;
    for (const sp::PinEvent& e : events)
      if (e.kind == kind) ++n;
    return n;
  }
  const sp::PinEvent* First(sp::PinEventKind kind) const {
    for (const sp::PinEvent& e : events)
      if (e.kind == kind) return &e;
    return nullptr;
  }
};

std::vector<std::string> g_fetches;
void RecordFetch(const std::string& id) { g_fetches.push_back(id); }

class MapSessionPin : public ::testing::Test {
 protected:
  void SetUp() override {
    sp::ResetForTest();
    g_fetches.clear();
  }
  void TearDown() override { sp::ResetForTest(); }

  // A client whose channel lists arena2 and whose mounted copy is version 1.0.
  static void ClientWithArena2() {
    sp::ChannelView view;
    view.listed = {{Level("arena2"), "arena2"}};
    sp::SetChannelView(view);
    sp::SetMountedPins({Pin("arena2", "1.0", kSourceA)});
    sp::SetFetchRequest(&RecordFetch);
  }
};

TEST_F(MapSessionPin, PinIsTheHashOfIdVersionAndTheSourcePrefix) {
  const sp::MapPin pin = Pin("arena2", "1.0", kSourceA);
  EXPECT_EQ(sp::PinValue(pin), EchoVR::CSymbol64Hash("arena2@1.0@0123456789abcdef"));
  EXPECT_NE(sp::PinValue(pin), 0u);
  EXPECT_NE(sp::PinValue(pin), EchoVR::CSymbol64Hash("arena2@1.0"));
}

TEST_F(MapSessionPin, PinIsEqualAcrossPlatforms) {
  const sp::MapPin pc = Pin("arena2", "1.0", kSourceA, "windows-x86_64");
  const sp::MapPin quest = Pin("arena2", "1.0", kSourceA, "android-arm64");
  ASSERT_NE(pc.index_sha256, quest.index_sha256);
  EXPECT_EQ(sp::PinValue(pc), sp::PinValue(quest));
}

TEST_F(MapSessionPin, TwoSourcesWithOneIdAndVersionGiveDifferentPins) {
  EXPECT_NE(sp::PinValue(Pin("arena2", "1.0", kSourceA)), sp::PinValue(Pin("arena2", "1.0", kSourceB)));
}

// The vectors the PC and the Quest were tested against (the Quest's level names, and the PC's pin test vector).
TEST_F(MapSessionPin, LevelNamesAreTheRadHashOfTheIdAndOfTheLowspecName) {
  EXPECT_EQ(sp::SessionLevelOf("mpl_lobby_b2"), 0xd09afd15b1c75c04ULL);
  EXPECT_EQ(sp::LowspecLevelOf("mpl_lobby_b2"), 0x05361c73bb73db19ULL);
  EXPECT_EQ(sp::LowspecLevelOf("mpl_arena_a"), 0x7691d78d414c5701ULL);
  EXPECT_NE(sp::SessionLevelOf("arena2"), sp::LowspecLevelOf("arena2"));
}

TEST_F(MapSessionPin, ServerPinIsZeroUnlessTheLevelIsAMountedMap) {
  sp::SetMountedPins({Pin("arena2", "1.0", kSourceA)});
  EXPECT_EQ(sp::ServerPinFor(Level("arena2")), sp::PinValue(Pin("arena2", "1.0", kSourceA)));
  EXPECT_EQ(sp::ServerPinFor(kStockLevel), 0u);
  EXPECT_EQ(sp::ServerPinFor(0), 0u);
}

TEST_F(MapSessionPin, AStockOrUnlistedLevelIsLeftAlone) {
  ClientWithArena2();
  EXPECT_EQ(sp::DecideClient(kStockLevel, 0x1234).action, sp::ClientAction::kNone);
  EXPECT_EQ(sp::DecideClient(Level("arena3"), 0x1234).action, sp::ClientAction::kNone);
  EXPECT_EQ(sp::DecideClient(0, 0x1234).action, sp::ClientAction::kNone);
}

TEST_F(MapSessionPin, AnAbsentPinIsUnpinned) {
  ClientWithArena2();
  const sp::ClientDecision decision = sp::DecideClient(Level("arena2"), 0);
  EXPECT_EQ(decision.action, sp::ClientAction::kUnpinned);
  EXPECT_EQ(decision.reason, "map_unpinned");
  EXPECT_FALSE(decision.request_fetch);
}

// A server that states no pin never makes the client leave, but nothing mounts a cached map at boot: a session on a level this client lists and
// holds on disk (not_asked) asks for the map's mount, and the load waits for it like a pinned one (ADR 0048 decision 16). A map that is
// not cached, or is refused, is a stay with no request: there is nothing a mount could cure.
TEST_F(MapSessionPin, AnUnpinnedSessionOnACachedMapRequestsItsMountAndNeverLeaves) {
  sp::ChannelView view;
  view.listed = {{Level("arena_c"), "arena_c"}, {Level("arena_d"), "arena_d"}, {Level("arena_e"), "arena_e"}};
  view.not_mounted = {{"arena_c", nevr_maps::kNotAskedToken}, {"arena_d", nevr_maps::kNotCachedToken}, {"arena_e", "bad_script"}};
  sp::SetChannelView(view);
  const sp::ClientDecision cached = sp::DecideClient(Level("arena_c"), 0);
  EXPECT_EQ(cached.action, sp::ClientAction::kUnpinned);
  EXPECT_TRUE(cached.request_fetch);
  EXPECT_TRUE(cached.holdable);
  EXPECT_TRUE(cached.cached);
  EXPECT_EQ(cached.notice_line, "Loading arena_c; join again when it is ready.");
  EXPECT_FALSE(sp::DecideClient(Level("arena_d"), 0).request_fetch);
  EXPECT_FALSE(sp::DecideClient(Level("arena_e"), 0).request_fetch);
}

TEST_F(MapSessionPin, TheMountedOrStagedPinMatches) {
  ClientWithArena2();
  EXPECT_EQ(sp::DecideClient(Level("arena2"), sp::PinValue(Pin("arena2", "1.0", kSourceA))).action, sp::ClientAction::kNone);

  sp::ResetForTest();
  sp::ChannelView view;
  view.listed = {{Level("arena2"), "arena2"}};
  view.staged = {Pin("arena2", "1.1", kSourceB)};
  sp::SetChannelView(view);
  EXPECT_EQ(sp::DecideClient(Level("arena2"), sp::PinValue(Pin("arena2", "1.1", kSourceB))).action, sp::ClientAction::kNone);
}

// The decision row that replaced `not_subscribed`: a listed map is one this client follows (decision 16), so a mismatch leaves
// with the version reason and a fetch. No config key is consulted.
TEST_F(MapSessionPin, AnotherVersionOfAListedMapLeavesAndRequestsAFetch) {
  ClientWithArena2();
  const sp::ClientDecision decision = sp::DecideClient(Level("arena2"), sp::PinValue(Pin("arena2", "1.1", kSourceB)));
  EXPECT_EQ(decision.action, sp::ClientAction::kLeave);
  EXPECT_EQ(decision.reason, "version");
  EXPECT_TRUE(decision.request_fetch);
  EXPECT_EQ(decision.notice_line, "Needs arena2 (not your 1.0); updating for next launch.");
}

TEST_F(MapSessionPin, AListedMapWithNoCopyLeavesAndRequestsAFetch) {
  sp::ChannelView view;
  view.listed = {{Level("arena2"), "arena2"}};
  sp::SetChannelView(view);
  const sp::ClientDecision decision = sp::DecideClient(Level("arena2"), kPinOfNothing);
  EXPECT_EQ(decision.action, sp::ClientAction::kLeave);
  EXPECT_EQ(decision.reason, "version");
  EXPECT_TRUE(decision.request_fetch);
  EXPECT_EQ(decision.notice_line, "Needs map arena2; updating for your next launch.");
}

TEST_F(MapSessionPin, ALocalCopyStaysWithoutAFetch) {
  sp::ChannelView view;
  view.listed = {{Level("arena2"), "arena2"}};
  view.local = {{"arena2", "C:/game/_local/arena2.kfclvl"}};
  sp::SetChannelView(view);
  const sp::ClientDecision decision = sp::DecideClient(Level("arena2"), kPinOfNothing);
  EXPECT_EQ(decision.action, sp::ClientAction::kLocal);
  EXPECT_EQ(decision.reason, "local_map");
  EXPECT_EQ(decision.local_path, "C:/game/_local/arena2.kfclvl");
  EXPECT_FALSE(decision.request_fetch);
  EXPECT_TRUE(decision.notice_line.empty());
}

TEST_F(MapSessionPin, EachNotMountedTokenLeavesAndOnlyStockChangedFetches) {
  for (const char* token : {"refused", "boot_budget", "stock_changed", "derived_stock_missing", "derived_stock_mismatch",
                            "derived_unsupported"}) {
    sp::ResetForTest();
    sp::ChannelView view;
    view.listed = {{Level("a2"), "a2"}};
    view.not_mounted = {{"a2", token}};
    sp::SetChannelView(view);
    const sp::ClientDecision decision = sp::DecideClient(Level("a2"), kPinOfNothing);
    EXPECT_EQ(decision.action, sp::ClientAction::kLeave) << token;
    EXPECT_EQ(decision.reason, "not_mounted") << token;
    EXPECT_EQ(decision.request_fetch, std::string(token) == "stock_changed") << token;
    EXPECT_EQ(decision.notice_line, "Map a2 did not load: " + std::string(token) + ". See the nevr log.") << token;
  }
}

TEST_F(MapSessionPin, EveryNoticeFitsTheLineForA55CharacterId) {
  const std::string id = "a" + std::string(54, 'b');
  ASSERT_EQ(id.size(), 55u);
  const std::uint64_t level = Level(id);
  std::vector<sp::ChannelView> views(3);
  for (sp::ChannelView& view : views) view.listed = {{level, id}};
  views[1].staged = {Pin(id, "10.20.30-beta.long", kSourceA)};
  views[2].not_mounted = {{id, "derived_stock_mismatch"}};
  for (const sp::ChannelView& view : views) {
    sp::ResetForTest();
    sp::SetChannelView(view);
    const sp::ClientDecision decision = sp::DecideClient(level, kPinOfNothing);
    ASSERT_EQ(decision.action, sp::ClientAction::kLeave);
    EXPECT_LE(decision.notice_line.size(), sp::kNoticeLineMax) << decision.notice_line;
    EXPECT_NE(decision.notice_line.find('~'), std::string::npos) << decision.notice_line;
    EXPECT_EQ(decision.notice_line.find(id), std::string::npos) << decision.notice_line;
  }
}

TEST_F(MapSessionPin, ViewFromMountListsBothLevelNamesAndTheStagedMaps) {
  nevr_maps::MountReport report;
  report.listed = {{Level("arena2"), "arena2"}};
  report.not_mounted = {{"dock", "refused"}};
  nevr_maps::MountedMap staged;
  staged.id = "arena2";
  staged.version = "1.0";
  staged.source_sha256 = kSourceA;
  staged.platform = "windows-x86_64";
  staged.level = Level("arena2");
  report.staged = {staged};
  const sp::ChannelView view = sp::ViewFromMount(report);
  ASSERT_EQ(view.listed.size(), 2u);
  EXPECT_EQ(view.listed[0], std::make_pair(sp::SessionLevelOf("arena2"), std::string("arena2")));
  EXPECT_EQ(view.listed[1], std::make_pair(sp::LowspecLevelOf("arena2"), std::string("arena2")));
  ASSERT_EQ(view.staged.size(), 1u);
  EXPECT_EQ(view.staged[0].version, "1.0");
  EXPECT_EQ(view.not_mounted, report.not_mounted);
}

// Replacement, not accumulation: the second call supersedes the first (call-twice tests).
TEST_F(MapSessionPin, SetChannelViewReplacesTheLastView) {
  sp::ChannelView first;
  first.listed = {{Level("arena2"), "arena2"}};
  first.staged = {Pin("arena2", "1.0", kSourceA)};
  sp::SetChannelView(first);
  ASSERT_EQ(sp::DecideClient(Level("arena2"), 0x55).action, sp::ClientAction::kLeave);

  sp::ChannelView second;
  second.listed = {{Level("dock"), "dock"}};
  sp::SetChannelView(second);
  EXPECT_EQ(sp::DecideClient(Level("arena2"), 0x55).action, sp::ClientAction::kNone);
  EXPECT_EQ(sp::DecideClient(Level("dock"), 0x55).action, sp::ClientAction::kLeave);
  EXPECT_TRUE(sp::LoginEntries().empty());
}

TEST_F(MapSessionPin, SetMountedPinsReplacesTheLastSet) {
  sp::SetMountedPins({Pin("arena2", "1.0", kSourceA)});
  sp::SetMountedPins({Pin("dock", "2.0", kSourceB)});
  ASSERT_EQ(sp::MountedPins().size(), 1u);
  EXPECT_EQ(sp::MountedPins()[0].id, "dock");
  EXPECT_EQ(sp::ServerPinFor(Level("arena2")), 0u);
  EXPECT_NE(sp::ServerPinFor(Level("dock")), 0u);
}

TEST_F(MapSessionPin, TheServerSetsThePinOnlyOnChange) {
  sp::SetMountedPins({Pin("arena2", "1.0", kSourceA)});
  FakeGame game;
  game.server = true;
  game.owner["level"] = Level("arena2");
  for (int i = 0; i < 100; ++i) sp::TickWith(game);
  EXPECT_EQ(game.set_calls, 1);
  EXPECT_EQ(game.owner["nevr_map_pin"], sp::PinValue(Pin("arena2", "1.0", kSourceA)));
  game.owner["level"] = kStockLevel;
  for (int i = 0; i < 100; ++i) sp::TickWith(game);
  EXPECT_EQ(game.set_calls, 2);
  EXPECT_EQ(game.owner["nevr_map_pin"], 0u);
  EXPECT_EQ(game.Count(sp::PinEventKind::kServerSet), 1u);
  EXPECT_EQ(game.Count(sp::PinEventKind::kServerCleared), 1u);
}

TEST_F(MapSessionPin, TheServerNeverClearsAValueItDidNotSet) {
  FakeGame game;
  game.server = true;
  game.owner["level"] = kStockLevel;
  game.owner["nevr_map_pin"] = 0x99;
  sp::SetMountedPins({Pin("arena2", "1.0", kSourceA)});  // mounted, but not for this level
  for (int i = 0; i < 10; ++i) sp::TickWith(game);
  EXPECT_EQ(game.set_calls, 0);
  EXPECT_EQ(game.owner["nevr_map_pin"], 0x99u);
}

TEST_F(MapSessionPin, WithNoMapNothingIsSetOrLeft) {
  FakeGame server;
  server.server = true;
  server.owner["level"] = kStockLevel;
  FakeGame client;
  client.owner["level"] = kStockLevel;
  client.owner["nevr_map_pin"] = kPinOfNothing;
  for (int i = 0; i < 10; ++i) {
    sp::TickWith(server);
    sp::TickWith(client);
  }
  EXPECT_EQ(server.set_calls, 0);
  EXPECT_EQ(client.set_calls + client.schedule_calls, 0);
  EXPECT_TRUE(client.notices.empty());
}

// The empty-state gate: with nothing listed and nothing mounted, the tick reads no owner JSON and calls no game function,
// so a per-frame tick costs one Callable check. A mutant that drops the gate fails this test.
TEST_F(MapSessionPin, AnEmptyStateMakesNoGameCall) {
  FakeGame server;
  server.server = true;
  server.owner["level"] = Level("arena2");
  FakeGame client;
  client.owner["level"] = Level("arena2");
  client.owner["nevr_map_pin"] = kPinOfNothing;
  for (int i = 0; i < 50; ++i) {
    sp::TickWith(server);
    sp::TickWith(client);
  }
  EXPECT_EQ(server.game_calls, 0);
  EXPECT_EQ(client.game_calls, 0);
}

TEST_F(MapSessionPin, AClientLeavesBeforeTheLoad) {
  ClientWithArena2();
  FakeGame game;
  game.state = 6;
  game.main_level = 0;  // LoadMap has not read "level" yet
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = sp::PinValue(Pin("arena2", "1.1", kSourceB));
  for (int i = 0; i < 20; ++i) sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 1);
  ASSERT_EQ(game.notices.size(), 1u);
  EXPECT_EQ(game.notices[0].first, "Map version mismatch");
  EXPECT_EQ(game.notices[0].second, "Needs arena2 (not your 1.0); updating for next launch.");
  EXPECT_EQ(g_fetches, std::vector<std::string>{"arena2"});
  const sp::PinEvent* skew = game.First(sp::PinEventKind::kSkew);
  ASSERT_NE(skew, nullptr);
  EXPECT_EQ(skew->reason, "version");
  EXPECT_EQ(skew->id, "arena2");
  EXPECT_TRUE(skew->fetch);
  EXPECT_EQ(game.Count(sp::PinEventKind::kLeaveScheduled), 1u);
}

TEST_F(MapSessionPin, AClientThatSeesThePinAfterTheLoadLeavesThroughTheSamePath) {
  ClientWithArena2();
  FakeGame game;
  game.main_level = Level("arena2");  // loaded, and the owner JSON has no level
  for (int i = 0; i < 5; ++i) sp::TickWith(game);
  EXPECT_EQ(game.Count(sp::PinEventKind::kUnpinned), 1u);
  EXPECT_EQ(game.schedule_calls, 0);

  game.owner["nevr_map_pin"] = sp::PinValue(Pin("arena2", "1.1", kSourceB));
  for (int i = 0; i < 5; ++i) sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 1);
  ASSERT_EQ(game.notices.size(), 1u);
  EXPECT_EQ(game.notices[0].second, "Needs arena2 (not your 1.0); updating for next launch.");
}

TEST_F(MapSessionPin, AFailingLeaveIsRetriedForTenSecondsThenLeaveFailed) {
  ClientWithArena2();
  FakeGame game;
  game.schedule_ok = false;
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = kPinOfNothing;
  sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 1);
  // Ticks 100 ms apart; the level changing meanwhile does not cancel the leave.
  for (int i = 1; i <= 100; ++i) {
    game.now += 100;
    if (i == 50) game.owner["level"] = kStockLevel;
    sp::TickWith(game);
  }
  EXPECT_EQ(game.schedule_calls, 101);
  EXPECT_EQ(game.Count(sp::PinEventKind::kLeaveFailed), 1u);
  for (int i = 0; i < 50; ++i) {
    game.now += 100;
    sp::TickWith(game);
  }
  EXPECT_EQ(game.schedule_calls, 101);
  EXPECT_EQ(game.Count(sp::PinEventKind::kLeaveFailed), 1u);
}

TEST_F(MapSessionPin, ALeaveThatSucceedsOnARetryStops) {
  ClientWithArena2();
  FakeGame game;
  game.schedule_ok = false;
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = kPinOfNothing;
  sp::TickWith(game);
  game.now += 100;
  game.schedule_ok = true;
  sp::TickWith(game);
  for (int i = 0; i < 20; ++i) {
    game.now += 100;
    sp::TickWith(game);
  }
  EXPECT_EQ(game.schedule_calls, 2);
  EXPECT_EQ(game.Count(sp::PinEventKind::kLeaveFailed), 0u);
}

TEST_F(MapSessionPin, ANewSessionIsDecidedAgain) {
  ClientWithArena2();
  FakeGame game;
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = kPinOfNothing;
  sp::TickWith(game);
  sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 1);
  game.state = 5;  // back in the lobby menus
  sp::TickWith(game);
  game.state = 9;
  sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 2);
}

TEST_F(MapSessionPin, NoLobbyOrMenusDoNothing) {
  ClientWithArena2();
  FakeGame game;
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = kPinOfNothing;
  game.lobby = 0;
  sp::TickWith(game);
  game.lobby = kLobby;
  game.state = 5;
  sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 0);
  EXPECT_TRUE(game.notices.empty());
}

TEST_F(MapSessionPin, ALocalMapStaysAndWarnsOncePerSession) {
  sp::ChannelView view;
  view.listed = {{Level("arena2"), "arena2"}};
  view.local = {{"arena2", "C:/game/_local/arena2.kfclvl"}};
  sp::SetChannelView(view);
  sp::SetFetchRequest(&RecordFetch);
  FakeGame game;
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = kPinOfNothing;
  for (int i = 0; i < 10; ++i) sp::TickWith(game);
  game.owner["nevr_map_pin"] = 0x66;
  for (int i = 0; i < 10; ++i) sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 0);
  EXPECT_TRUE(game.notices.empty());
  EXPECT_TRUE(g_fetches.empty());
  EXPECT_EQ(game.Count(sp::PinEventKind::kLocalMap), 1u);
  EXPECT_EQ(game.First(sp::PinEventKind::kLocalMap)->local_path, "C:/game/_local/arena2.kfclvl");
}

TEST_F(MapSessionPin, AnUncallableGameIsNeverCalled) {
  ClientWithArena2();
  FakeGame game;
  game.callable = false;
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = kPinOfNothing;
  sp::TickWith(game);
  game.server = true;
  sp::TickWith(game);
  EXPECT_EQ(game.game_calls + game.set_calls + game.schedule_calls, 0);
}

TEST_F(MapSessionPin, SessionLevelPrefersTheOwnerLevel) {
  FakeGame game;
  game.main_level = kStockLevel;
  EXPECT_EQ(sp::SessionLevelWith(game), kStockLevel);
  game.owner["level"] = Level("arena2");
  EXPECT_EQ(sp::SessionLevelWith(game), Level("arena2"));
  game.lobby = 0;
  EXPECT_EQ(sp::SessionLevelWith(game), kStockLevel);
}

TEST_F(MapSessionPin, WithNothingStagedTheLoginIsEmpty) {
  EXPECT_EQ(sp::LoginEntries(), nlohmann::json::array());
  EXPECT_TRUE(sp::RegistrationEntries().empty());
}

TEST_F(MapSessionPin, TheLoginCarriesStagedAndMountedMaps) {
  sp::ChannelView view;
  view.staged = {Pin("arena2", "1.0", kSourceA), Pin("dock", "2.0", kSourceB)};
  sp::SetChannelView(view);
  sp::SetMountedPins({Pin("arena2", "1.0", kSourceA)});

  const nlohmann::json entries = sp::LoginEntries();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0]["id"], "arena2");
  EXPECT_EQ(entries[0]["mounted"], true);
  EXPECT_EQ(entries[0]["source_sha256"], kSourceA);
  EXPECT_EQ(entries[0]["platform"], "windows-x86_64");
  EXPECT_EQ(entries[0]["version"], "1.0");
  EXPECT_EQ(entries[0]["index_sha256"], std::string(64, '1'));
  // The level is "0x" and sixteen lowercase hex digits (the login's spelling).
  char level[19];
  std::snprintf(level, sizeof(level), "0x%016llx", static_cast<unsigned long long>(Level("arena2")));
  EXPECT_EQ(entries[0]["level"], std::string(level));
  EXPECT_EQ(entries[1]["id"], "dock");
  EXPECT_EQ(entries[1]["mounted"], false);

  const std::vector<std::string> registration = sp::RegistrationEntries();
  ASSERT_EQ(registration.size(), 2u);
  for (const std::string& entry : registration) EXPECT_FALSE(nlohmann::json::parse(entry).contains("mounted"));
}

TEST_F(MapSessionPin, MountedPinsIsSafeDuringSetMountedPins) {
  std::atomic<bool> stop{false};
  std::thread writer([&stop] {
    for (int i = 0; i < 2000 && !stop; ++i)
      sp::SetMountedPins(i % 2 ? std::vector<sp::MapPin>{Pin("arena2", "1.0", kSourceA), Pin("dock", "2.0", kSourceB)}
                               : std::vector<sp::MapPin>{});
    stop = true;
  });
  std::size_t reads = 0, torn = 0;
  while (!stop) {
    const std::vector<sp::MapPin> pins = sp::MountedPins();
    if (!(pins.empty() || (pins.size() == 2 && pins[0].id == "arena2" && pins[1].id == "dock"))) ++torn;
    ++reads;
  }
  writer.join();
  EXPECT_GT(reads, 0u);
  EXPECT_EQ(torn, 0u);
}

TEST_F(MapSessionPin, LoginAndRegistrationEntriesAreSafeDuringSetMountedPins) {
  sp::ChannelView view;
  view.staged = {Pin("arena2", "1.0", kSourceA), Pin("dock", "2.0", kSourceB)};
  sp::SetChannelView(view);
  std::atomic<bool> stop{false};
  std::thread writer([&stop] {
    for (int i = 0; i < 2000 && !stop; ++i)
      sp::SetMountedPins(i % 2 ? std::vector<sp::MapPin>{Pin("arena2", "1.0", kSourceA)} : std::vector<sp::MapPin>{});
    stop = true;
  });
  std::size_t reads = 0, torn = 0;
  while (!stop) {
    // Mounted or not, each staged map appears once.
    if (sp::LoginEntries().size() != 2u || sp::RegistrationEntries().size() != 2u) ++torn;
    ++reads;
  }
  writer.join();
  EXPECT_GT(reads, 0u);
  EXPECT_EQ(torn, 0u);
}

// The fields of a record for a pin that is off. A module or build check names only the reason; a byte check names the function, its
// VA, then the expected and the actual bytes, in that order (logging.md Rule 6).
TEST(MapSessionPinRecord, AModuleCheckStatesOnlyTheReasonAndTheConsequence) {
  const sp::PinFields fields = sp::PinOffFields("no_game_module", nullptr, 0, nullptr, nullptr, 0);
  ASSERT_EQ(fields.size(), 3u);
  EXPECT_EQ(fields[0], (sp::PinField{"status", "off"}));
  EXPECT_EQ(fields[1], (sp::PinField{"reason", "no_game_module"}));
  EXPECT_EQ(fields[2].first, "consequence");
}

TEST(MapSessionPinRecord, AByteCheckNamesTheFunctionTheVaAndBothByteRunsInOrder) {
  const std::uint8_t expected[] = {0x01, 0x02};
  const std::uint8_t actual[] = {0xab, 0xcd};
  const sp::PinFields fields = sp::PinOffFields("owner_getter_mismatch", "fn", 0x1234, expected, actual, 2);
  ASSERT_EQ(fields.size(), 7u);
  EXPECT_EQ(fields[2], (sp::PinField{"name", "fn"}));
  EXPECT_EQ(fields[3], (sp::PinField{"va", "0x0000000000001234"}));
  EXPECT_EQ(fields[4], (sp::PinField{"expected", "0102"}));
  EXPECT_EQ(fields[5], (sp::PinField{"actual", "abcd"}));
  EXPECT_EQ(fields[6].first, "consequence");
}

TEST(MapSessionPinRecord, ACheckPassesOnAgreementAndFillsTheRefusalOnDisagreement) {
  const std::uint8_t want[] = {0x10, 0x20};
  std::uint8_t at[] = {0x10, 0x20};
  sp::PinFields refusal;
  EXPECT_TRUE(sp::CheckGameBytes(at, 0x99, want, 2, sp::SameBytesExact, "r", "n", refusal));
  EXPECT_TRUE(refusal.empty());
  at[1] = 0x21;
  EXPECT_FALSE(sp::CheckGameBytes(at, 0x99, want, 2, sp::SameBytesExact, "r", "n", refusal));
  ASSERT_EQ(refusal.size(), 7u);
  EXPECT_EQ(refusal[1].second, "r");
  EXPECT_EQ(refusal[4].second, "1020");  // expected
  EXPECT_EQ(refusal[5].second, "1021");  // actual
}

TEST(MapSessionPinRecord, TheLineQuotesTheConsequenceAndNothingElse) {
  const sp::PinFields fields = sp::PinOffFields("no_game_module", nullptr, 0, nullptr, nullptr, 0);
  EXPECT_EQ(sp::FormatFields(fields), "status=off reason=no_game_module consequence=\"a mismatched server is not left\"");
}

}  // namespace

// R11 (PLAN-v7 5.5 "One leave owner", "Feed", tests 1-8): the cargo pin's two inputs on the session pin's one tick, with the real
// decision (core/cargo_session_pin.h OutcomeOf) and a fake game. A platform that sets no input has the map pin's tick unchanged.
namespace {

namespace ncp = nevr_cargo_pin;

constexpr std::uint64_t kCargoB = 0x1111111111111111ull;  // this client's or this server's set
constexpr std::uint64_t kCargoX = 0x2222222222222222ull;  // another set

// A client whose boot set is whole with pin `local` (the view a PC binding gives, PLAN-v7 5.5 "The decision").
ncp::CargoView WholeSet(std::uint64_t local) {
  ncp::CargoView view;
  view.local = local;
  view.status.state = ncp::CargoBoot::kWhole;
  return view;
}

sp::CargoDecideFn Decide(const ncp::CargoView& view) {
  return [view](std::uint64_t level, std::uint64_t server_pin) { return ncp::OutcomeOf(view, level, server_pin); };
}

}  // namespace

// Test 1 (G2): a server with no map mounted writes its cargo pin on change, and the map pin's early return does not stop it.
TEST_F(MapSessionPin, TheServerWritesItsCargoPinOnChangeEvenWithNoMapPin) {
  FakeGame server;
  server.server = true;
  sp::SetCargoServerPin(kCargoB);
  sp::TickWith(server);
  sp::TickWith(server);
  EXPECT_EQ(server.owner[sp::kOwnerCargoPinKey], kCargoB);
  EXPECT_EQ(server.set_calls, 1);
  EXPECT_EQ(server.Count(sp::PinEventKind::kCargoServerSet), 1u);
}

// Test 1b (F7, G2): a server with no whole set writes kCargoPinNone, not 0: "no set" is a fact a client acts on.
TEST_F(MapSessionPin, AServerWithNoWholeSetWritesNoneNotAbsent) {
  FakeGame server;
  server.server = true;
  sp::SetCargoServerPin(ncp::kCargoPinNone);
  sp::TickWith(server);
  EXPECT_EQ(server.owner[sp::kOwnerCargoPinKey], ncp::kCargoPinNone);
  EXPECT_NE(server.owner[sp::kOwnerCargoPinKey], 0u);
}

// Test 1c: with no cargo input (the Quest, and the PC before its feed) the tick writes no cargo key and records no decision.
TEST_F(MapSessionPin, WithNoCargoInputTheTickWritesNoCargoKeyAndDecidesNoCargo) {
  ClientWithArena2();
  FakeGame server;
  server.server = true;
  server.owner["level"] = Level("arena2");
  sp::TickWith(server);
  EXPECT_EQ(server.owner.count(sp::kOwnerCargoPinKey), 0u);
  FakeGame client;
  client.owner["level"] = Level("arena2");
  client.owner["nevr_map_pin"] = kPinOfNothing;
  sp::TickWith(client);
  EXPECT_EQ(client.Count(sp::PinEventKind::kSessionDecision), 0u);
  EXPECT_EQ(client.Count(sp::PinEventKind::kCargoSkew), 0u);
  EXPECT_EQ(client.Count(sp::PinEventKind::kSkew), 1u);
}

// Test 2 (F8): the first read of the server's pin is absent (the stay), a later read is another set: the client leaves then. The
// decision is made again when the pin changes, not once per session.
TEST_F(MapSessionPin, AClientThatReadsAPinLaterLeavesOnIt) {
  FakeGame client;
  client.owner["level"] = kStockLevel;
  sp::SetCargoDecide(Decide(WholeSet(kCargoB)));
  sp::TickWith(client);
  EXPECT_EQ(client.schedule_calls, 0);
  ASSERT_EQ(client.Count(sp::PinEventKind::kSessionDecision), 1u);
  EXPECT_EQ(client.First(sp::PinEventKind::kSessionDecision)->cargo_reason, "unpinned");
  EXPECT_EQ(client.First(sp::PinEventKind::kSessionDecision)->leave, "none");
  client.owner[sp::kOwnerCargoPinKey] = kCargoX;
  sp::TickWith(client);
  EXPECT_EQ(client.schedule_calls, 1);
  EXPECT_EQ(client.Count(sp::PinEventKind::kCargoSkew), 1u);
  ASSERT_EQ(client.notices.size(), 1u);
  EXPECT_EQ(client.notices[0].first, ncp::kCargoNoticeTitle);
  EXPECT_EQ(client.notices[0].second, "EchoCombat differs from this server: restart the game to update");
  // The decision line of this key names the cargo as the input that left (PLAN-v7 5.5 "The decision": leave=cargo).
  const sp::PinEvent* leaving = nullptr;
  for (const sp::PinEvent& e : client.events)
    if (e.kind == sp::PinEventKind::kSessionDecision) leaving = &e;
  ASSERT_NE(leaving, nullptr);
  EXPECT_EQ(leaving->leave, "cargo");
  EXPECT_NE(leaving->cargo_reason, "none");
  // The leave is taken: the same key again makes no decision and no second leave.
  sp::TickWith(client);
  EXPECT_EQ(client.schedule_calls, 1);
}

// Test 2b: the decision line is written once per key, not once per frame.
TEST_F(MapSessionPin, TheDecisionLineIsWrittenOncePerKey) {
  FakeGame client;
  client.owner["level"] = kStockLevel;
  client.owner[sp::kOwnerCargoPinKey] = kCargoB;
  sp::SetCargoDecide(Decide(WholeSet(kCargoB)));
  constexpr int kTicksOnOneKey = 5;  // frames in one session with no key change: one decision line for all of them
  for (int i = 0; i < kTicksOnOneKey; ++i) sp::TickWith(client);
  ASSERT_EQ(client.Count(sp::PinEventKind::kSessionDecision), 1u);
  EXPECT_EQ(client.First(sp::PinEventKind::kSessionDecision)->cargo_reason, "none");
}

// Test 3 (X1): a client with no set in a pinned Arena session stays: Arena is not a combat level.
TEST_F(MapSessionPin, AnArenaSessionWithNoSetStays) {
  FakeGame client;
  client.owner["level"] = Level("mpl_arena_a");
  client.owner[sp::kOwnerCargoPinKey] = ncp::kCargoPinNone;
  sp::SetCargoDecide(Decide(ncp::CargoView()));
  sp::TickWith(client);
  EXPECT_EQ(client.schedule_calls, 0);
  EXPECT_EQ(client.Count(sp::PinEventKind::kCargoSkew), 0u);
  EXPECT_TRUE(client.notices.empty());
}

// Test 4 (F6, row 11): a client with no set in a combat session whose server's pin is the manifest's, not downloaded: leaves with
// row 11's line.
TEST_F(MapSessionPin, ACombatSessionWithNoSetLeavesWithTheNotDownloadedLine) {
  FakeGame client;
  client.owner["level"] = kStockLevel;
  client.owner[sp::kOwnerCargoPinKey] = kCargoX;
  ncp::CargoView view;
  view.manifest_known = true;
  view.manifest = kCargoX;
  sp::SetCargoDecide(Decide(view));
  sp::TickWith(client);
  EXPECT_EQ(client.schedule_calls, 1);
  ASSERT_EQ(client.notices.size(), 1u);
  EXPECT_EQ(client.notices[0].second, "EchoCombat not downloaded yet: restart to play this match");
  ASSERT_EQ(client.Count(sp::PinEventKind::kCargoSkew), 1u);
  EXPECT_EQ(client.First(sp::PinEventKind::kCargoSkew)->reason, "not_downloaded");
}

// Test 5 (F6): the two pebbles levels are compared as the four combat maps are.
TEST_F(MapSessionPin, ThePebblesLevelsLeaveOnAnotherSet) {
  for (const std::uint64_t level : {ncp::kCombatPebblesLevel, ncp::kCombatPebblesPayloadLevel}) {
    FakeGame client;
    client.owner["level"] = level;
    client.owner[sp::kOwnerCargoPinKey] = kCargoX;
    sp::SetCargoDecide(Decide(WholeSet(kCargoB)));
    sp::TickWith(client);
    EXPECT_EQ(client.schedule_calls, 1) << "level " << level;
  }
}

// Test 6 (F6): the combat lobby is not in the scope: a session on mpl_lobby_b2 stays.
TEST_F(MapSessionPin, TheLobbyStaysWhateverTheSets) {
  FakeGame client;
  client.owner["level"] = Level("mpl_lobby_b2");
  client.owner[sp::kOwnerCargoPinKey] = kCargoX;
  sp::SetCargoDecide(Decide(WholeSet(kCargoB)));
  sp::TickWith(client);
  EXPECT_EQ(client.schedule_calls, 0);
  EXPECT_EQ(client.Count(sp::PinEventKind::kCargoSkew), 0u);
}

// Test 7: a map leave and a cargo leave together give one leave, the map's notice, and one decision line naming both inputs.
TEST_F(MapSessionPin, AMapAndACargoLeaveGiveOneLeaveAndTheMapsNotice) {
  sp::ChannelView view;
  view.listed = {{kStockLevel, "mpl_combat_dyson"}};
  sp::SetChannelView(view);
  sp::SetMountedPins({Pin("mpl_combat_dyson", "1.0", kSourceA)});
  sp::SetFetchRequest(&RecordFetch);
  FakeGame client;
  client.owner["level"] = kStockLevel;
  client.owner["nevr_map_pin"] = kPinOfNothing;
  client.owner[sp::kOwnerCargoPinKey] = kCargoX;
  sp::SetCargoDecide(Decide(WholeSet(kCargoB)));
  sp::TickWith(client);
  EXPECT_EQ(client.schedule_calls, 1);
  ASSERT_EQ(client.notices.size(), 1u);
  EXPECT_EQ(client.notices[0].first, sp::kNoticeTitle);
  EXPECT_EQ(client.Count(sp::PinEventKind::kCargoSkew), 0u);
  EXPECT_EQ(client.Count(sp::PinEventKind::kSkew), 1u);
  ASSERT_EQ(client.Count(sp::PinEventKind::kSessionDecision), 1u);
  const sp::PinEvent* decision = client.First(sp::PinEventKind::kSessionDecision);
  EXPECT_EQ(decision->reason, "version");
  EXPECT_EQ(decision->cargo_reason, "unchecked");
  EXPECT_EQ(decision->leave, "map");
  EXPECT_EQ(g_fetches.size(), 1u);
}


// Test 9 (PLAN-v7 5.5 R11): row 14 asks for the refetch marker, and the tick decides once per key, so a session with an unknown manifest
// asks for it once however many frames it lasts; a key change (here, a pin the server writes later, F8) asks again.
TEST_F(MapSessionPin, RowFourteenAsksForTheMarkerOncePerKey) {
  int asks = 0;
  sp::SetCargoDecide([&asks](std::uint64_t level, std::uint64_t server_pin) {
    ncp::CargoView view;
    view.local = kCargoB;
    view.status.state = ncp::CargoBoot::kWhole;
    view.manifest_known = false;  // M unknown: row 14
    const ncp::CargoDecision decision = ncp::DecideCargo(view, level, server_pin);
    if (decision.write_marker) ++asks;
    return ncp::OutcomeFrom(view, decision);
  });
  FakeGame client;
  client.owner["level"] = kStockLevel;
  client.owner[sp::kOwnerCargoPinKey] = kCargoX;
  for (int i = 0; i < 4; ++i) sp::TickWith(client);
  EXPECT_EQ(asks, 1);

  client.owner[sp::kOwnerCargoPinKey] = kCargoX + 1;
  sp::TickWith(client);
  EXPECT_EQ(asks, 2);
}

// The request entry's rows (ADR 0048 decision 16): a listed map the client does not hold leaves with a download notice and asks for
// it; a stale copy is never played in its place, so it leaves the same way.
TEST_F(MapSessionPin, AListedMapNotCachedLeavesWithADownloadNoticeAndRequestsIt) {
  sp::ChannelView view;
  view.listed = {{Level("arena_c"), "arena_c"}};
  view.not_mounted = {{"arena_c", "not_cached"}};
  sp::SetChannelView(view);
  const sp::ClientDecision decision = sp::DecideClient(Level("arena_c"), sp::PinValue(Pin("arena_c", "1.0", kSourceA)));
  EXPECT_EQ(decision.action, sp::ClientAction::kLeave);
  EXPECT_EQ(decision.reason, "not_mounted");
  EXPECT_TRUE(decision.request_fetch);
  EXPECT_EQ(decision.notice_line, "Downloading arena_c; join again when it is ready.");
}

// A cached, current map no session asked for yet (the on-demand boot lists every cached map and mounts none): the session asks for it, and the
// notice says it loads, since nothing is downloaded (ADR 0048 decision 16).
TEST_F(MapSessionPin, ACachedMapNoSessionAskedForYetLeavesAndRequestsItsMount) {
  sp::ChannelView view;
  view.listed = {{Level("arena_c"), "arena_c"}};
  view.not_mounted = {{"arena_c", nevr_maps::kNotAskedToken}};
  sp::SetChannelView(view);
  const sp::ClientDecision decision = sp::DecideClient(Level("arena_c"), sp::PinValue(Pin("arena_c", "1.0", kSourceA)));
  EXPECT_EQ(decision.action, sp::ClientAction::kLeave);
  EXPECT_EQ(decision.reason, "not_mounted");
  EXPECT_TRUE(decision.request_fetch);
  EXPECT_TRUE(decision.holdable);
  EXPECT_EQ(decision.notice_line, "Loading arena_c; join again when it is ready.");
}

// Only a map stored at the listed version and not yet mounted awaits its mount: the request for it needs no fetch (core/map_request.h).
TEST_F(MapSessionPin, OnlyACachedCurrentMapAwaitsItsMount) {
  sp::ChannelView view;
  view.listed = {{Level("a"), "a"}, {Level("b"), "b"}, {Level("c"), "c"}, {Level("d"), "d"}};
  view.not_mounted = {{"a", nevr_maps::kNotAskedToken}, {"b", nevr_maps::kNotCachedToken}, {"c", nevr_maps::kStaleToken}, {"d", "bad_script"}};
  sp::SetChannelView(view);
  EXPECT_TRUE(sp::CachedMapAwaitsMount("a"));
  EXPECT_FALSE(sp::CachedMapAwaitsMount("b"));  // not on disk: a download
  EXPECT_FALSE(sp::CachedMapAwaitsMount("c"));  // on disk at a version no manifest lists: a download
  EXPECT_FALSE(sp::CachedMapAwaitsMount("d"));  // refused: a fetch cannot cure it
  EXPECT_FALSE(sp::CachedMapAwaitsMount("unlisted"));
}

TEST_F(MapSessionPin, AStaleCopyLeavesAndRequestsTheListedVersion) {
  sp::ChannelView view;
  view.listed = {{Level("arena_c"), "arena_c"}};
  view.not_mounted = {{"arena_c", "stale"}};
  sp::SetChannelView(view);
  const sp::ClientDecision decision = sp::DecideClient(Level("arena_c"), sp::PinValue(Pin("arena_c", "1.1", kSourceA)));
  EXPECT_EQ(decision.action, sp::ClientAction::kLeave);
  EXPECT_TRUE(decision.request_fetch);
  EXPECT_EQ(decision.notice_line, "Downloading arena_c; join again when it is ready.");
}

// The client's join hold (KFC-PORT K4): the game's level load (CR15NetGame::LoadMap) is skipped while the map the session pins is
// fetched and mounted; the tick then replays the load, or leaves through the one leave (ClientLeaveOnce) with the notice.
class MapJoinHold : public MapSessionPin {
 protected:
  // A client that can hold its load is in a session on arena_c 1.0, which its channel lists and its cache lacks.
  void ArenaCSession(const std::string& token = "not_cached", std::int32_t state = 5) {
    sp::ChannelView view;
    view.listed = {{Level("arena_c"), "arena_c"}};
    if (!token.empty()) view.not_mounted = {{"arena_c", token}};
    sp::SetChannelView(view);
    sp::SetFetchRequest(&RecordFetch);
    game.can_hold = true;
    game.state = state;  // 5: the direct path (an ingame session) reaches LoadMap before the game parks in state 6
    game.owner[sp::kOwnerLevelKey] = Level("arena_c");
    game.owner[sp::kOwnerPinKey] = kPin10;
  }
  // The worker mounted the map: the view names it, and the registry's mount completed.
  static void Mounted() {
    sp::ChannelView view;
    view.listed = {{Level("arena_c"), "arena_c"}};
    view.staged = {Pin("arena_c", "1.0", kSourceA)};
    sp::SetChannelView(view);
    sp::NoteMapsMounted({"arena_c"});
  }
  std::uint64_t HoldTimeout(const nevr_maps::HoldPlan& plan) const { return nevr_maps::MapHoldTimeoutMs(plan); }

  const std::uint64_t kPin10 = sp::PinValue(Pin("arena_c", "1.0", kSourceA));
  FakeGame game;
};

TEST_F(MapJoinHold, AMapTheCacheLacksSkipsTheLoadAsksForItAndTheMountReplaysTheSameLoad) {
  ArenaCSession();
  EXPECT_TRUE(sp::HoldLoadBegin(game));
  ASSERT_EQ(game.Count(sp::PinEventKind::kHoldArmed), 1u);
  EXPECT_EQ(g_fetches, std::vector<std::string>{"arena_c"});
  EXPECT_EQ(game.schedule_calls, 0);
  EXPECT_TRUE(game.notices.empty());
  EXPECT_EQ(game.replays, 0);

  // The game calls LoadMap again (Update, state 6): the hold stands, nothing is asked twice.
  EXPECT_TRUE(sp::HoldLoadBegin(game));
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldArmed), 1u);
  EXPECT_EQ(g_fetches.size(), 1u);

  // The tick runs in any game state while a load is held (5 here), and a view that names the map is not enough: the registry's mount is.
  sp::ChannelView staged;
  staged.listed = {{Level("arena_c"), "arena_c"}};
  staged.staged = {Pin("arena_c", "1.0", kSourceA)};
  sp::SetChannelView(staged);
  sp::TickWith(game);
  EXPECT_EQ(game.replays, 0);

  Mounted();
  sp::TickWith(game);
  EXPECT_EQ(game.replays, 1);
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldReleased), 1u);
  EXPECT_EQ(game.schedule_calls, 0);
  sp::TickWith(game);
  EXPECT_EQ(game.replays, 1);  // once
}

TEST_F(MapJoinHold, TheHoldIsAStateTheLeaveOwnerReadsNotASecondLeave) {
  ArenaCSession("not_cached", 6);
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  sp::TickWith(game);  // state 6 is a session state: the pin's decision would leave here, but a held load decides nothing
  EXPECT_EQ(game.schedule_calls, 0);
  EXPECT_EQ(game.Count(sp::PinEventKind::kSkew), 0u);
  EXPECT_TRUE(game.notices.empty());
}

TEST_F(MapJoinHold, ATimeoutLeavesThroughTheOneLeaveWithThePlainNotice) {
  ArenaCSession();
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  game.now += nevr_maps::kMapHoldCeilingMs - 1;  // no plan yet: the ceiling
  sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 0);
  game.now += 1;
  sp::TickWith(game);
  const sp::PinEvent* leave = game.First(sp::PinEventKind::kSkew);
  ASSERT_NE(leave, nullptr);
  EXPECT_EQ(leave->reason, "hold_timeout");
  EXPECT_FALSE(leave->fetch);
  EXPECT_EQ(game.schedule_calls, 1);
  ASSERT_EQ(game.notices.size(), 1u);
  EXPECT_EQ(game.notices[0].second, "Downloading arena_c took too long; join again later.");
  EXPECT_EQ(game.replays, 0);
  EXPECT_EQ(g_fetches.size(), 1u);  // the leave asks for nothing more
  EXPECT_TRUE(sp::HoldLoadBegin(game));  // the game's retry of the load is still skipped while the leave runs
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldArmed), 1u);
}

TEST_F(MapJoinHold, ALeaveTheHoldTookIsNotDecidedAgainByThePinsOwnTick) {
  ArenaCSession();
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  game.now += nevr_maps::kMapHoldCeilingMs;
  sp::TickWith(game);
  ASSERT_EQ(game.Count(sp::PinEventKind::kSkew), 1u);
  game.state = 6;  // the game parks in state 6 after the skipped load: the pin's own tick now runs
  sp::TickWith(game);
  sp::TickWith(game);
  EXPECT_EQ(game.Count(sp::PinEventKind::kSkew), 1u);  // one leave owner, one record for the session
}

TEST_F(MapJoinHold, AnotherLevelInTheSameLobbyDropsTheHold) {
  ArenaCSession();
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  game.owner[sp::kOwnerLevelKey] = Level("arena_d");
  sp::TickWith(game);
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldDropped), 1u);
  EXPECT_EQ(game.schedule_calls, 0);
}

TEST_F(MapJoinHold, TheTimeoutFollowsTheMeasuredDownloadAndNeverExceedsTheCeiling) {
  ArenaCSession();
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  nevr_maps::HoldPlan plan;
  plan.planned = true;
  plan.set_bytes = 100;
  plan.bytes_to_fetch = 100;
  plan.bytes_fetched = 100;  // nothing left to download: the floor
  plan.elapsed_ms = 1000;
  sp::NoteHoldProgress(plan);
  ASSERT_EQ(HoldTimeout(plan), nevr_maps::kMapHoldFloorMs);
  game.now += nevr_maps::kMapHoldFloorMs - 1;
  sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 0);
  game.now += 1;
  sp::TickWith(game);
  const sp::PinEvent* leave = game.First(sp::PinEventKind::kSkew);
  ASSERT_NE(leave, nullptr);
  EXPECT_EQ(leave->reason, "hold_timeout");
}

TEST_F(MapJoinHold, AFailedFetchOfTheHeldMapLeavesAtOnceAndAnotherMapsFailureDoesNot) {
  ArenaCSession();
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  sp::NoteFetchFailed("arena_d", "no such map");
  sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 0);
  sp::NoteFetchFailed("arena_c", "not enough room for arena_c");
  sp::TickWith(game);
  const sp::PinEvent* leave = game.First(sp::PinEventKind::kSkew);
  ASSERT_NE(leave, nullptr);
  EXPECT_EQ(leave->reason, "fetch_failed");
  EXPECT_EQ(game.schedule_calls, 1);
  ASSERT_EQ(game.notices.size(), 1u);
  EXPECT_EQ(game.notices[0].second, "Map arena_c could not be downloaded. See the nevr log.");
  EXPECT_EQ(game.replays, 0);
}

TEST_F(MapJoinHold, AMountOfAnotherVersionIsNeverReplayedInto) {
  ArenaCSession();
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  sp::ChannelView view;
  view.listed = {{Level("arena_c"), "arena_c"}};
  view.staged = {Pin("arena_c", "1.1", kSourceB)};  // the bucket moved on: the server still runs 1.0
  sp::SetChannelView(view);
  sp::NoteMapsMounted({"arena_c"});
  sp::TickWith(game);
  EXPECT_EQ(game.replays, 0);
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldReleased), 0u);
  const sp::PinEvent* leave = game.First(sp::PinEventKind::kSkew);
  ASSERT_NE(leave, nullptr);
  EXPECT_EQ(leave->reason, "version");
  EXPECT_EQ(game.schedule_calls, 1);
}

TEST_F(MapJoinHold, AMountThatCompletedBeforeTheHoldDoesNotReleaseIt) {
  sp::NoteMapsMounted({"arena_c"});  // an earlier request's mount
  ArenaCSession();
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  sp::ChannelView view;
  view.listed = {{Level("arena_c"), "arena_c"}};
  view.staged = {Pin("arena_c", "1.0", kSourceA)};
  sp::SetChannelView(view);
  sp::TickWith(game);
  EXPECT_EQ(game.replays, 0);
}

TEST_F(MapJoinHold, AReplayTheGameRefusesLeavesWithAPlainReason) {
  ArenaCSession();
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  game.replay_ok = false;
  Mounted();
  sp::TickWith(game);
  EXPECT_EQ(game.replays, 1);
  const sp::PinEvent* leave = game.First(sp::PinEventKind::kSkew);
  ASSERT_NE(leave, nullptr);
  EXPECT_EQ(leave->reason, "replay_failed");
  EXPECT_EQ(game.schedule_calls, 1);
}

TEST_F(MapJoinHold, AnotherSessionDropsTheHoldWithoutALeave) {
  ArenaCSession();
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  game.lobby = kLobby + 1;
  sp::TickWith(game);
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldDropped), 1u);
  EXPECT_EQ(game.schedule_calls, 0);
  EXPECT_EQ(game.replays, 0);
  Mounted();
  game.lobby = kLobby;
  sp::TickWith(game);
  EXPECT_EQ(game.replays, 0);  // dropped: the late mount replays nothing
}

TEST_F(MapJoinHold, AStaleCopyAndACachedMapNoSessionAskedForAreHeldLikeAMapTheCacheLacks) {
  // A copy the channel no longer lists is fetched anew, so the join waits for it (ADR 0048 decision 16).
  ArenaCSession(nevr_maps::kStaleToken);
  EXPECT_TRUE(sp::HoldLoadBegin(game));
  // A cached map no session asked for (every cached map of an on-demand boot) is mounted for this join, so the join waits for the mount.
  sp::ResetForTest();
  g_fetches.clear();
  ArenaCSession(nevr_maps::kNotAskedToken);
  EXPECT_TRUE(sp::HoldLoadBegin(game));
  EXPECT_EQ(g_fetches.size(), 1u);  // and the one request that mounts it goes out
  // Nothing downloads for it, so the hold's notice says it loads.
  const std::string armed = game.events.back().notice;
  EXPECT_EQ(armed, "Loading arena_c; the join continues when it is ready.");
}

// An unpinned session on a cached map is held for the mount and replayed once it mounts, as a pinned one is; a platform that cannot hold the
// load still asks for the mount from the tick, and the client is never sent out of the session.
TEST_F(MapJoinHold, AnUnpinnedSessionOnACachedMapIsHeldForItsMountAndReplayed) {
  ArenaCSession(nevr_maps::kNotAskedToken);
  game.owner[sp::kOwnerPinKey] = 0;
  EXPECT_TRUE(sp::HoldLoadBegin(game));
  EXPECT_EQ(g_fetches, std::vector<std::string>{"arena_c"});
  sp::TickWith(game);
  EXPECT_EQ(game.replays, 0);  // still waiting for the mount, not released by the unpinned decision
  EXPECT_EQ(game.schedule_calls, 0);
  Mounted();
  sp::TickWith(game);
  EXPECT_EQ(game.replays, 1);
  EXPECT_EQ(game.schedule_calls, 0);
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldReleased), 1u);

  sp::ResetForTest();
  g_fetches.clear();
  FakeGame noHold;
  ArenaCSession(nevr_maps::kNotAskedToken);
  noHold.can_hold = false;
  noHold.owner[sp::kOwnerLevelKey] = Level("arena_c");
  noHold.owner[sp::kOwnerPinKey] = 0;
  EXPECT_FALSE(sp::HoldLoadBegin(noHold));
  sp::TickWith(noHold);
  EXPECT_EQ(g_fetches, std::vector<std::string>{"arena_c"});
  EXPECT_EQ(noHold.schedule_calls, 0);
}

// A hold for a map that is on disk and only needs its mount says so when it ends in a leave: nothing was downloading.
TEST_F(MapJoinHold, ATimeoutOrAFailureOnACachedMapSaysLoadingNotDownloading) {
  ArenaCSession(nevr_maps::kNotAskedToken);
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  game.now += nevr_maps::kMapHoldCeilingMs;
  sp::TickWith(game);
  ASSERT_EQ(game.notices.size(), 1u);
  EXPECT_EQ(game.notices[0].second, "Loading arena_c took too long; join again later.");

  sp::ResetForTest();
  g_fetches.clear();
  game.notices.clear();
  ArenaCSession(nevr_maps::kNotAskedToken);
  ASSERT_TRUE(sp::HoldLoadBegin(game));
  sp::NoteFetchFailed("arena_c", "the manifest check failed");
  sp::TickWith(game);
  ASSERT_EQ(game.notices.size(), 1u);
  EXPECT_EQ(game.notices[0].second, "Map arena_c could not be loaded. See the nevr log.");
}

TEST_F(MapJoinHold, WhatNoMountCanCureIsNotHeld) {
  // A platform that cannot hold, a server, a session whose map is current, another version already mounted, and a stay: the load runs.
  ArenaCSession();
  game.can_hold = false;
  EXPECT_FALSE(sp::HoldLoadBegin(game));
  game.can_hold = true;
  game.server = true;
  EXPECT_FALSE(sp::HoldLoadBegin(game));
  game.server = false;
  sp::SetMountedPins({Pin("arena_c", "0.9", kSourceB)});
  sp::ChannelView view;
  view.listed = {{Level("arena_c"), "arena_c"}};
  sp::SetChannelView(view);
  EXPECT_FALSE(sp::HoldLoadBegin(game));  // a mounted copy at another version stays mounted: a fetch cannot cure it
  sp::SetMountedPins({Pin("arena_c", "1.0", kSourceA)});
  EXPECT_FALSE(sp::HoldLoadBegin(game));  // the session's own version is mounted: nothing to wait for
  game.owner[sp::kOwnerLevelKey] = kStockLevel;
  EXPECT_FALSE(sp::HoldLoadBegin(game));  // a level the channel does not list
  EXPECT_EQ(game.Count(sp::PinEventKind::kHoldArmed), 0u);
  EXPECT_EQ(game.replays, 0);
}
