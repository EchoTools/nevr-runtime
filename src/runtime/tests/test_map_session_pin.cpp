#include "runtime/content/map_session_pin.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/logging.h"
#include "nevr/symbol_hash.h"
#include "runtime/compat/login_profile.h"
#include "runtime/content/map_session_pin_seam.h"

namespace {

std::mutex g_logMutex;
std::vector<std::string> g_logLines;

}  // namespace

VOID Log(EchoVR::LogLevel, const CHAR* format, ...) {
  char line[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_logLines.emplace_back(line);
}

namespace {

namespace sp = nevr_map_session_pin;

std::size_t LogCount(const std::string& needle) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  std::size_t n = 0;
  for (const std::string& line : g_logLines)
    if (line.find(needle) != std::string::npos) ++n;
  return n;
}

const std::string kSourceA = "0123456789abcdef" + std::string(48, 'a');
const std::string kSourceB = "fedcba9876543210" + std::string(48, 'b');

sp::MapPin Pin(const std::string& id, const std::string& version, const std::string& source,
               const char* platform = "windows-x86_64") {
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

class FakeGame : public sp::GameAccess {
 public:
  bool callable = true;
  bool server = false;
  std::uint64_t lobby = kLobby;
  std::int32_t state = 9;
  std::map<std::string, std::uint64_t> owner;
  std::uint64_t main_level = 0;
  bool schedule_ok = true;
  std::uint64_t now = 1000;
  int set_calls = 0;
  int schedule_calls = 0;
  int get_calls = 0;
  std::vector<std::pair<std::string, std::string>> notices;

  bool Callable() override { return callable; }
  bool IsServer() override { return server; }
  std::uint64_t Lobby() override { return lobby; }
  std::int32_t NetState() override { return state; }
  std::uint64_t OwnerSymbol(std::uint64_t, const char* key) override {
    ++get_calls;
    auto it = owner.find(key);
    return it == owner.end() ? 0 : it->second;
  }
  void SetOwnerSymbol(std::uint64_t, const char* key, std::uint64_t value) override {
    ++set_calls;
    owner[key] = value;
  }
  std::uint64_t MainLevel() override { return main_level; }
  bool WriteNotice(const char* title, const char* line) override {
    notices.emplace_back(title, line);
    return true;
  }
  bool ScheduleLeave() override {
    ++schedule_calls;
    return schedule_ok;
  }
  std::uint64_t NowMs() override { return now; }
};

std::vector<std::string> g_fetches;
void RecordFetch(const std::string& id) { g_fetches.push_back(id); }

class MapSessionPin : public ::testing::Test {
 protected:
  void SetUp() override {
    sp::ResetForTest();
    g_fetches.clear();
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logLines.clear();
  }
  void TearDown() override { sp::ResetForTest(); }

  // A client whose channel lists arena2, subscribed and mounted at 1.0.
  static void ClientWithArena2() {
    sp::ChannelView view;
    view.listed = {{Level("arena2"), "arena2"}};
    view.subscribed = {"arena2"};
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

TEST_F(MapSessionPin, TwoChannelsWithOneIdAndVersionGiveDifferentPins) {
  EXPECT_NE(sp::PinValue(Pin("arena2", "1.0", kSourceA)), sp::PinValue(Pin("arena2", "1.0", kSourceB)));
}

TEST_F(MapSessionPin, ServerPinIsZeroUnlessTheLevelIsAMountedMaps) {
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

TEST_F(MapSessionPin, TheMountedOrStagedPinMatches) {
  ClientWithArena2();
  EXPECT_EQ(sp::DecideClient(Level("arena2"), sp::PinValue(Pin("arena2", "1.0", kSourceA))).action,
            sp::ClientAction::kNone);

  sp::ResetForTest();
  sp::ChannelView view;
  view.listed = {{Level("arena2"), "arena2"}};
  view.subscribed = {"arena2"};
  view.staged = {Pin("arena2", "1.1", kSourceB)};
  sp::SetChannelView(view);
  EXPECT_EQ(sp::DecideClient(Level("arena2"), sp::PinValue(Pin("arena2", "1.1", kSourceB))).action,
            sp::ClientAction::kNone);
}

TEST_F(MapSessionPin, AnotherVersionWhenSubscribedLeavesAndRequestsAFetch) {
  ClientWithArena2();
  const sp::ClientDecision decision = sp::DecideClient(Level("arena2"), sp::PinValue(Pin("arena2", "1.1", kSourceB)));
  EXPECT_EQ(decision.action, sp::ClientAction::kLeave);
  EXPECT_EQ(decision.reason, "version");
  EXPECT_TRUE(decision.request_fetch);
  EXPECT_EQ(decision.notice_line, "Needs arena2 (not your 1.0); updating for next launch.");
}

TEST_F(MapSessionPin, NoCopyWhenSubscribedLeavesAndRequestsAFetch) {
  sp::ChannelView view;
  view.listed = {{Level("arena2"), "arena2"}};
  view.subscribed = {"arena2"};
  sp::SetChannelView(view);
  const sp::ClientDecision decision = sp::DecideClient(Level("arena2"), 0x55);
  EXPECT_EQ(decision.action, sp::ClientAction::kLeave);
  EXPECT_TRUE(decision.request_fetch);
  EXPECT_EQ(decision.notice_line, "Needs map arena2; updating for your next launch.");
}

TEST_F(MapSessionPin, NotSubscribedLeavesWithoutAFetch) {
  sp::ChannelView view;
  view.listed = {{Level("arena2"), "arena2"}};
  sp::SetChannelView(view);
  const sp::ClientDecision decision = sp::DecideClient(Level("arena2"), 0x55);
  EXPECT_EQ(decision.action, sp::ClientAction::kLeave);
  EXPECT_EQ(decision.reason, "not_subscribed");
  EXPECT_FALSE(decision.request_fetch);
  EXPECT_EQ(decision.notice_line, "Needs map arena2. Add it to maps.subscribed.");
}

TEST_F(MapSessionPin, ALocalCopyStaysWithoutAFetch) {
  sp::ChannelView view;
  view.listed = {{Level("arena2"), "arena2"}};
  view.subscribed = {"arena2"};
  view.local = {{"arena2", "C:/game/_local/arena2.kfclvl"}};
  sp::SetChannelView(view);
  const sp::ClientDecision decision = sp::DecideClient(Level("arena2"), 0x55);
  EXPECT_EQ(decision.action, sp::ClientAction::kLocal);
  EXPECT_EQ(decision.reason, "local_map");
  EXPECT_FALSE(decision.request_fetch);
  EXPECT_TRUE(decision.notice_line.empty());
}

TEST_F(MapSessionPin, EachNotMountedTokenLeavesAndOnlyStockChangedFetches) {
  for (const char* token : {"refused", "boot_budget", "stock_changed", "derived_stock_missing", "derived_stock_mismatch",
                            "derived_unsupported"}) {
    // A short id, so the line with the longest token fits kNoticeLineMax whole; EveryNoticeFitsTheLine... covers the cut.
    sp::ResetForTest();
    sp::ChannelView view;
    view.listed = {{Level("a2"), "a2"}};
    view.subscribed = {"a2"};
    view.not_mounted = {{"a2", token}};
    sp::SetChannelView(view);
    const sp::ClientDecision decision = sp::DecideClient(Level("a2"), 0x55);
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
  std::vector<sp::ChannelView> views(4);
  for (sp::ChannelView& view : views) view.listed = {{level, id}};
  views[0].subscribed = {id};
  views[1].subscribed = {id};
  views[1].staged = {Pin(id, "10.20.30-beta.long", kSourceA)};
  views[2].not_mounted = {{id, "derived_stock_mismatch"}};
  for (const sp::ChannelView& view : views) {
    sp::ResetForTest();
    sp::SetChannelView(view);
    const sp::ClientDecision decision = sp::DecideClient(level, 0x55);
    ASSERT_EQ(decision.action, sp::ClientAction::kLeave);
    EXPECT_LE(decision.notice_line.size(), sp::kNoticeLineMax) << decision.notice_line;
    EXPECT_NE(decision.notice_line.find('~'), std::string::npos) << decision.notice_line;
    EXPECT_EQ(decision.notice_line.find(id), std::string::npos) << decision.notice_line;
  }
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
}

TEST_F(MapSessionPin, TheServerNeverClearsAValueItDidNotSet) {
  FakeGame game;
  game.server = true;
  game.owner["level"] = kStockLevel;
  game.owner["nevr_map_pin"] = 0x99;
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
  client.owner["nevr_map_pin"] = 0x55;
  for (int i = 0; i < 10; ++i) {
    sp::TickWith(server);
    sp::TickWith(client);
  }
  EXPECT_EQ(server.set_calls, 0);
  EXPECT_EQ(client.set_calls + client.schedule_calls, 0);
  EXPECT_TRUE(client.notices.empty());
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
  EXPECT_EQ(LogCount("map_skew reason=version id=arena2"), 1u);
}

TEST_F(MapSessionPin, AClientThatSeesThePinAfterTheLoadLeavesThroughTheSamePath) {
  ClientWithArena2();
  FakeGame game;
  game.main_level = Level("arena2");  // loaded, and the owner JSON has no level
  for (int i = 0; i < 5; ++i) sp::TickWith(game);
  EXPECT_EQ(LogCount("map_unpinned id=arena2"), 1u);
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
  game.owner["nevr_map_pin"] = 0x55;
  sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 1);
  // Ticks 100 ms apart; the level changing meanwhile does not cancel the leave.
  for (int i = 1; i <= 100; ++i) {
    game.now += 100;
    if (i == 50) game.owner["level"] = kStockLevel;
    sp::TickWith(game);
  }
  EXPECT_EQ(game.schedule_calls, 101);
  EXPECT_EQ(LogCount("map_skew reason=leave_failed id=arena2"), 1u);
  for (int i = 0; i < 50; ++i) {
    game.now += 100;
    sp::TickWith(game);
  }
  EXPECT_EQ(game.schedule_calls, 101);
  EXPECT_EQ(LogCount("map_skew reason=leave_failed"), 1u);
}

TEST_F(MapSessionPin, ALeaveThatSucceedsOnARetryStops) {
  ClientWithArena2();
  FakeGame game;
  game.schedule_ok = false;
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = 0x55;
  sp::TickWith(game);
  game.now += 100;
  game.schedule_ok = true;
  sp::TickWith(game);
  for (int i = 0; i < 20; ++i) {
    game.now += 100;
    sp::TickWith(game);
  }
  EXPECT_EQ(game.schedule_calls, 2);
  EXPECT_EQ(LogCount("leave_failed"), 0u);
}

TEST_F(MapSessionPin, ANewSessionIsDecidedAgain) {
  ClientWithArena2();
  FakeGame game;
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = 0x55;
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
  game.owner["nevr_map_pin"] = 0x55;
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
  view.subscribed = {"arena2"};
  view.local = {{"arena2", "C:/game/_local/arena2.kfclvl"}};
  sp::SetChannelView(view);
  sp::SetFetchRequest(&RecordFetch);
  FakeGame game;
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = 0x55;
  for (int i = 0; i < 10; ++i) sp::TickWith(game);
  game.owner["nevr_map_pin"] = 0x66;
  for (int i = 0; i < 10; ++i) sp::TickWith(game);
  EXPECT_EQ(game.schedule_calls, 0);
  EXPECT_TRUE(game.notices.empty());
  EXPECT_TRUE(g_fetches.empty());
  EXPECT_EQ(LogCount("map_skew reason=local_map id=arena2: local C:/game/_local/arena2.kfclvl overrides channel map"), 1u);
}

TEST_F(MapSessionPin, AnUncallableGameIsNeverCalled) {
  ClientWithArena2();
  FakeGame game;
  game.callable = false;
  game.owner["level"] = Level("arena2");
  game.owner["nevr_map_pin"] = 0x55;
  sp::TickWith(game);
  game.server = true;
  sp::TickWith(game);
  EXPECT_EQ(game.get_calls + game.set_calls + game.schedule_calls, 0);
}

TEST_F(MapSessionPin, SessionLevelPrefersTheOwnerLevel) {
  FakeGame game;
  game.main_level = kStockLevel;
  EXPECT_EQ(sp::SessionLevelOf(game), kStockLevel);
  game.owner["level"] = Level("arena2");
  EXPECT_EQ(sp::SessionLevelOf(game), Level("arena2"));
  game.lobby = 0;
  EXPECT_EQ(sp::SessionLevelOf(game), kStockLevel);
}

TEST_F(MapSessionPin, WithNothingStagedTheLoginIsUnchanged) {
  EXPECT_EQ(sp::LoginEntriesJson(), "[]");
  EXPECT_TRUE(sp::RegistrationEntries().empty());

  nevr_login_profile::LoginProfileInputs before;
  before.account_id = 42;
  before.display_name = "player";
  nevr_login_profile::LoginProfileInputs after = before;
  after.maps = nlohmann::json::parse(sp::LoginEntriesJson());
  EXPECT_EQ(nevr_login_profile::BuildLoginProfileJson(after), nevr_login_profile::BuildLoginProfileJson(before));

  const nlohmann::json profile = nlohmann::json::parse(nevr_login_profile::BuildLoginProfileJson(after));
  std::vector<std::string> keys;
  for (auto it = profile.begin(); it != profile.end(); ++it) keys.push_back(it.key());
  const std::vector<std::string> stock = {"access_token", "accountid",    "appid",          "buildversion",
                                          "bypassauth",   "desiredclientprofileversion",    "displayname",
                                          "hmdserialnumber", "lobbyversion", "nevr_identity", "nevr_plugins",
                                          "nevr_social",  "nonce",        "password",       "publisher_lock",
                                          "system_info"};
  EXPECT_EQ(keys, stock);
}

TEST_F(MapSessionPin, TheLoginCarriesStagedAndMountedMaps) {
  sp::ChannelView view;
  view.staged = {Pin("arena2", "1.0", kSourceA), Pin("dock", "2.0", kSourceB)};
  sp::SetChannelView(view);
  sp::SetMountedPins({Pin("arena2", "1.0", kSourceA)});

  const nlohmann::json entries = nlohmann::json::parse(sp::LoginEntriesJson());
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0]["id"], "arena2");
  EXPECT_EQ(entries[0]["mounted"], true);
  EXPECT_EQ(entries[0]["source_sha256"], kSourceA);
  EXPECT_EQ(entries[0]["platform"], "windows-x86_64");
  EXPECT_EQ(entries[0]["version"], "1.0");
  EXPECT_EQ(entries[0]["index_sha256"], std::string(64, '1'));
  char level[24];
  std::snprintf(level, sizeof(level), "0x%016llx", static_cast<unsigned long long>(Level("arena2")));
  EXPECT_EQ(entries[0]["level"], std::string(level));
  EXPECT_EQ(entries[1]["id"], "dock");
  EXPECT_EQ(entries[1]["mounted"], false);

  nevr_login_profile::LoginProfileInputs inputs;
  inputs.maps = entries;
  EXPECT_EQ(nlohmann::json::parse(nevr_login_profile::BuildLoginProfileJson(inputs))["nevr_maps"], entries);

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
    if (nlohmann::json::parse(sp::LoginEntriesJson()).size() != 2u || sp::RegistrationEntries().size() != 2u) ++torn;
    ++reads;
  }
  writer.join();
  EXPECT_GT(reads, 0u);
  EXPECT_EQ(torn, 0u);
}

}  // namespace
