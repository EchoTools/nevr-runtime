// The ConfigAlias detours (runtime/content/config_alias_hook.cpp) against stubs of the game's balance lookup, level
// Enable/Disable and rules refresh, over a fake game context whose level tables the test fills. The detours are
// called directly, as test_arena_rules_hook calls its detour; nothing is hooked and no game file is read.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "abi/echovr_functions.h"
#include "core/config_alias.h"
#include "core/globals.h"
#include "core/logging.h"
#include "runtime/content/config_alias_hook.h"
#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/config.h"

BOOL g_isServer = TRUE;  // the runtime defines it in cli.cpp
PVOID g_pGame = nullptr;

namespace {

std::atomic<unsigned long> g_loopThread{0};
std::atomic<bool> g_failNextAllocation{false};

}  // namespace

unsigned long GameLoopThreadId() { return g_loopThread.load(); }

// Behind noinline helpers so an optimising build cannot inline `free` into a caller that inlined `operator new`
// (-Wmismatched-new-delete), as in core/tests/config_alias_test.cpp.
__attribute__((noinline)) void* RawAllocate(std::size_t size) { return std::malloc(size == 0 ? 1 : size); }
__attribute__((noinline)) void RawFree(void* p) { std::free(p); }

void* operator new(std::size_t size) {
  if (g_failNextAllocation.exchange(false)) throw std::bad_alloc();
  if (void* p = RawAllocate(size)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { RawFree(p); }
void operator delete(void* p, std::size_t) noexcept { RawFree(p); }

FLOAT CJsonGetFloatHook(PVOID root, const CHAR* path, FLOAT defaultValue, INT32 required);

namespace {

using nevr_config_alias::kArenaRulesKey;

constexpr std::uint64_t kAlias = 0x6216cd74aba74633ull;  // rad_hash("m1_arena_rules")
constexpr std::uint64_t kMain = 0x1000, kSub = 0x1001, kStock = 0x9000, kOtherKey = 0x4242;

struct Record {
  FLOAT round_time;
};
Record g_stockRules{300.0f}, g_aliasRules{200.0f}, g_otherRecord{1.0f};

bool g_aliasRecordPresent = true;
std::vector<std::uint64_t> g_keysAsked;

void* GameLookup(std::uint64_t key) {
  g_keysAsked.push_back(key);
  if (key == kArenaRulesKey) return &g_stockRules;
  if (key == kAlias) return g_aliasRecordPresent ? &g_aliasRules : nullptr;
  if (key == kOtherKey) return &g_otherRecord;
  return nullptr;
}

FLOAT GameCJsonGetFloat(PVOID root, const CHAR*, FLOAT, INT32) { return static_cast<Record*>(root)->round_time; }

std::uint64_t g_mainLevel = 0;
std::uint64_t MainLevel() { return g_mainLevel; }

std::vector<std::string> g_lines;
VOID CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_lines.emplace_back(message); }

bool HoldsAll(void*, const std::string&, std::uint64_t) { return true; }

// The game context holds the net game at CONTEXT_NETGAME_OFFSET; the net game holds the game object at +8 and the
// client lobby at +0x40; the game object holds the two level tables. Nothing else is read.
std::vector<unsigned char> g_context(0x8600, 0);
std::vector<unsigned char> g_netGame(0x100, 0);
std::vector<unsigned char> g_gameObject(0x2000, 0);
std::vector<std::uint64_t> g_disabledSpaces;  // {level symbol, level resource} pairs
std::vector<std::uint64_t> g_enabledSpaces;

template <typename T>
void Put(std::vector<unsigned char>* bytes, std::uintptr_t offset, T value) {
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

void SetClientLobby(std::uintptr_t lobby) { Put(&g_netGame, 0x40, lobby); }

void WriteTable(const std::vector<std::uint64_t>& pairs, std::uintptr_t data_offset, std::uintptr_t count_offset) {
  const std::uint64_t* entries = pairs.empty() ? nullptr : pairs.data();
  Put(&g_gameObject, data_offset, entries);
  Put(&g_gameObject, count_offset, static_cast<std::uint64_t>(pairs.size() / 2));
}

// The loaded levels that are not enabled (game object +0x1F90).
void SetLoadedLevels(const std::vector<std::uint64_t>& levels) {
  g_disabledSpaces.clear();
  for (const std::uint64_t level : levels) {
    g_disabledSpaces.push_back(level);
    g_disabledSpaces.push_back(0x7700 + level);  // a non-null resource
  }
  WriteTable(g_disabledSpaces, nevr_patch_addresses::GAME_SPACE_TABLE_OFFSET,
             nevr_patch_addresses::GAME_SPACE_TABLE_COUNT_OFFSET);
}

void SetEnabledSpaces(const std::vector<std::pair<std::uint64_t, const void*>>& spaces) {
  g_enabledSpaces.clear();
  for (const auto& space : spaces) {
    g_enabledSpaces.push_back(space.first);
    g_enabledSpaces.push_back(reinterpret_cast<std::uintptr_t>(space.second));
  }
  WriteTable(g_enabledSpaces, nevr_patch_addresses::GAME_ENABLED_SPACE_TABLE_OFFSET,
             nevr_patch_addresses::GAME_ENABLED_SPACE_TABLE_COUNT_OFFSET);
}

// A count no level table has: the game's tables read as unreadable and source B decides.
void BreakTheTables() {
  static const std::uint64_t kSomewhere[2] = {kStock, 0x7700};
  Put(&g_gameObject, nevr_patch_addresses::GAME_SPACE_TABLE_OFFSET, &kSomewhere[0]);
  Put(&g_gameObject, nevr_patch_addresses::GAME_SPACE_TABLE_COUNT_OFFSET, std::uint64_t{5000});
}

// A level resource: its level symbol at +0x58 and its flags at +0xC40.
struct FakeResource {
  std::vector<unsigned char> bytes = std::vector<unsigned char>(0xC48, 0);
  explicit FakeResource(std::uint64_t level) { Put(&bytes, nevr_patch_addresses::LEVEL_RESOURCE_LEVEL_OFFSET, level); }
  void* get() { return bytes.data(); }
  void SetDisabled() {
    Put(&bytes, nevr_patch_addresses::LEVEL_RESOURCE_FLAGS_OFFSET, nevr_patch_addresses::LEVEL_RESOURCE_DISABLED_FLAG);
  }
};

std::int64_t g_levelResult = 0;
std::int64_t GameEnable(void*) { return g_levelResult; }
std::int64_t GameDisable(void*) { return g_levelResult; }

std::vector<void*> g_refreshes;
void* g_readInRefresh = nullptr;
std::int64_t GameRefresh(void* net_game) {
  g_refreshes.push_back(net_game);
  g_readInRefresh = nevr_config_alias_hook::LookupDetour(kArenaRulesKey);
  return 0;
}

class ConfigAliasHookTest : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_config_alias_hook::ResetForTesting();
    nevr_config_alias_hook::SetOriginalForTesting(&GameLookup);
    nevr_config_alias_hook::SetLevelResourceOriginalsForTesting(&GameEnable, &GameDisable);
    nevr_config_alias_hook::SetRefreshForTesting(&GameRefresh);
    nevr_config_alias::Unstage("owner-m1");
    Put(&g_context, nevr_patch_addresses::CONTEXT_NETGAME_OFFSET, static_cast<void*>(g_netGame.data()));
    Put(&g_netGame, nevr_patch_addresses::NETGAME_GAME_OFFSET, static_cast<void*>(g_gameObject.data()));
    SetClientLobby(0xc1000);
    g_pGame = g_context.data();
    SetLoadedLevels({});
    SetEnabledSpaces({});
    g_loopThread = GetCurrentThreadId();
    g_mainLevel = 0;
    g_aliasRecordPresent = true;
    g_levelResult = 0;
    g_refreshes.clear();
    g_readInRefresh = nullptr;
    g_keysAsked.clear();
    g_lines.clear();
    SetBootLogSink(&CaptureLog);
  }
  void TearDown() override {
    SetBootLogSink(nullptr);
    nevr_config_alias::Unstage("owner-m1");
  }

  static void StageAndArm() {
    ASSERT_EQ(nevr_config_alias::Stage("owner-m1", "m1", kMain, {{kArenaRulesKey, kAlias}}), 1u);
    ASSERT_EQ(nevr_config_alias::Arm({{"owner-m1", {kMain, kSub}}}, &HoldsAll, nullptr), 1u);
  }

  std::size_t LinesWith(const std::string& text) const {
    std::size_t n = 0;
    for (const std::string& line : g_lines) n += line.find(text) != std::string::npos ? 1 : 0;
    return n;
  }
};

TEST_F(ConfigAliasHookTest, WithNoAliasEveryLookupIsTheGamesOwn) {
  SetLoadedLevels({kStock});
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kOtherKey), &g_otherRecord);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(0x5555), nullptr);
  EXPECT_EQ(g_keysAsked, (std::vector<std::uint64_t>{kArenaRulesKey, kOtherKey, 0x5555}));
  EXPECT_TRUE(g_lines.empty());
}

TEST_F(ConfigAliasHookTest, ASublevelInTheGameSpaceTableAliasesTheKeyInTheSameFrame) {
  StageAndArm();
  SetLoadedLevels({kStock});
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
  SetLoadedLevels({kStock, kSub});
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kOtherKey), &g_otherRecord);
  SetLoadedLevels({kStock});
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
}

TEST_F(ConfigAliasHookTest, TheMainLevelComesFromTheInstalledSource) {
  StageAndArm();
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
  nevr_config_alias_hook::SetMainLevelSource(&MainLevel);
  g_mainLevel = kMain;
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  g_mainLevel = 0;
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
}

TEST_F(ConfigAliasHookTest, OffTheGameThreadTheLookupReadsThePublishedSnapshotOnly) {
  StageAndArm();
  SetLoadedLevels({kSub});
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  SetLoadedLevels({});
  void* fromOtherThread = nullptr;
  std::thread([&] { fromOtherThread = nevr_config_alias_hook::LookupDetour(kArenaRulesKey); }).join();
  EXPECT_EQ(fromOtherThread, &g_aliasRules);
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
}

TEST_F(ConfigAliasHookTest, ArmingAfterTheLevelLoadedTakesEffectAtTheNextLookup) {
  SetLoadedLevels({kSub});
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  StageAndArm();  // the table is unchanged; the armed maps changed
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
}

TEST_F(ConfigAliasHookTest, AnAbsentAliasRecordFallsBackToStockAndIsLoggedOnce) {
  StageAndArm();
  SetLoadedLevels({kSub});
  g_aliasRecordPresent = false;
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
  EXPECT_EQ(LinesWith("[NEVR.MAPS] config_alias reason=alias_not_found"), 1u);
}

TEST_F(ConfigAliasHookTest, AFailedReconcileKeepsTheOldSnapshotAndLogsOnce) {
  StageAndArm();
  SetLoadedLevels({kSub});
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  ASSERT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  SetLoadedLevels({kStock});
  g_failNextAllocation = true;
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  EXPECT_FALSE(g_failNextAllocation.load());
  EXPECT_EQ(nevr_config_alias::Resolve(kArenaRulesKey), kAlias);
  g_failNextAllocation = true;
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  EXPECT_EQ(LinesWith("[NEVR.MAPS] config_alias reason=reconcile_failed"), 1u);
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  EXPECT_EQ(nevr_config_alias::Resolve(kArenaRulesKey), kArenaRulesKey);
}

// The config.yaml arena.round_time still wins over a map's aliased rules (docs/adr/0048 decision 5): the alias picks which
// record is read; the CJson_GetFloat detour then overrides the value by its path.
TEST_F(ConfigAliasHookTest, ConfiguredRoundTimeWinsOverAnActiveAlias) {
  EchoVR::CJsonGetFloat = &GameCJsonGetFloat;
  StageAndArm();
  SetLoadedLevels({kSub});
  PVOID root = nevr_config_alias_hook::LookupDetour(kArenaRulesKey);
  ASSERT_EQ(root, &g_aliasRules);
  g_arenaRoundTime = 0.0f;
  EXPECT_EQ(CJsonGetFloatHook(root, "round_time", 0.0f, 0), 200.0f);
  g_arenaRoundTime = 240.0f;
  EXPECT_EQ(CJsonGetFloatHook(root, "round_time", 0.0f, 0), 240.0f);
  g_arenaRoundTime = 0.0f;
}

// EnableGameSpace moves a level from the +0x1F90 table to the +0x1F50 one: an enabled sublevel is still loaded.
TEST_F(ConfigAliasHookTest, ALevelInTheEnabledTableCountsAsLoaded) {
  StageAndArm();
  FakeResource sub(kSub);
  SetEnabledSpaces({{kSub, sub.get()}});
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  SetEnabledSpaces({});
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
}

TEST_F(ConfigAliasHookTest, WhileTheTablesCanBeReadAnEnableEventAloneAddsNoLevel) {
  StageAndArm();
  FakeResource sub(kSub);
  EXPECT_EQ(nevr_config_alias_hook::EnableDetour(sub.get()), 0);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
  SetEnabledSpaces({{kSub, sub.get()}});
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
}

TEST_F(ConfigAliasHookTest, WithoutReadableTablesEnableAndDisableDecide) {
  StageAndArm();
  BreakTheTables();
  FakeResource sub(kSub);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
  nevr_config_alias_hook::EnableDetour(sub.get());
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  EXPECT_EQ(LinesWith("[NEVR.MAPS] config_alias source=level_events"), 1u);
  nevr_config_alias_hook::DisableDetour(sub.get());
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
}

TEST_F(ConfigAliasHookTest, AFailedEnableOrDisableIsReturnedAndNotRecorded) {
  StageAndArm();
  BreakTheTables();
  FakeResource sub(kSub);
  g_levelResult = 7;
  EXPECT_EQ(nevr_config_alias_hook::EnableDetour(sub.get()), 7);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
  g_levelResult = 0;
  nevr_config_alias_hook::EnableDetour(sub.get());
  g_levelResult = 7;
  EXPECT_EQ(nevr_config_alias_hook::DisableDetour(sub.get()), 7);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
}

// Source B: a resource torn down without Disable drops out at the next reconcile.
TEST_F(ConfigAliasHookTest, AResourceThatStopsReadingEnabledDropsOutWithoutADisable) {
  StageAndArm();
  BreakTheTables();
  FakeResource sub(kSub);
  nevr_config_alias_hook::EnableDetour(sub.get());
  ASSERT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  sub.SetDisabled();
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
  FakeResource reused(kSub);
  nevr_config_alias_hook::EnableDetour(reused.get());
  ASSERT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  Put(&reused.bytes, nevr_patch_addresses::LEVEL_RESOURCE_LEVEL_OFFSET, kStock);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
}

TEST_F(ConfigAliasHookTest, ASessionChangeClearsTheLevelEvents) {
  StageAndArm();
  BreakTheTables();
  FakeResource sub(kSub);
  nevr_config_alias_hook::EnableDetour(sub.get());
  ASSERT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  SetClientLobby(0xc2000);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
  SetClientLobby(0xc1000);
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_stockRules);
}

TEST_F(ConfigAliasHookTest, TheRefreshRunsFromTheTickWhenTheWinnerChanges) {
  nevr_config_alias_hook::SetCachedRulesRefresh(true);
  StageAndArm();
  SetLoadedLevels({kSub});
  EXPECT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  EXPECT_TRUE(g_refreshes.empty());
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  ASSERT_EQ(g_refreshes, (std::vector<void*>{g_netGame.data()}));
  EXPECT_EQ(g_readInRefresh, &g_aliasRules);
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  EXPECT_EQ(g_refreshes.size(), 1u);
  SetLoadedLevels({});
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  EXPECT_EQ(g_refreshes.size(), 2u);
  EXPECT_EQ(g_readInRefresh, &g_stockRules);
  EXPECT_EQ(LinesWith("[NEVR.MAPS] config_alias refresh key=mp_arena_rules"), 2u);
}

TEST_F(ConfigAliasHookTest, WithTheRefreshOffTheGameIsNeverCalled) {
  StageAndArm();
  SetLoadedLevels({kSub});
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  SetLoadedLevels({});
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  EXPECT_TRUE(g_refreshes.empty());
}

// config.yaml's round_time wins over a live alias (decision 5), and the win is logged: the line names the map, the key,
// the value the game read from the aliased record and config.yaml's value, once per map and key per load.
TEST_F(ConfigAliasHookTest, AnAliasLiveOverrideLogsOncePerLoadAndReturnsTheConfigValue) {
  EchoVR::CJsonGetFloat = &GameCJsonGetFloat;
  StageAndArm();
  SetLoadedLevels({kSub});
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  PVOID root = nevr_config_alias_hook::LookupDetour(kArenaRulesKey);
  ASSERT_EQ(root, &g_aliasRules);
  g_arenaRoundTime = 240.0f;
  EXPECT_EQ(CJsonGetFloatHook(root, "round_time", 0.0f, 0), 240.0f);
  EXPECT_EQ(CJsonGetFloatHook(root, "round_time", 0.0f, 0), 240.0f);
  EXPECT_EQ(LinesWith("[NEVR.MAPS] config_alias override key=round_time map=m1 map_value=200.000 config_value=240.000"),
            1u);
  EXPECT_EQ(LinesWith("config_alias override"), 1u);
  g_arenaRoundTime = 0.0f;
}

// A new load (the owner mounted again) logs the override again: the line is once per load, not once per process.
TEST_F(ConfigAliasHookTest, AnOverrideIsLoggedAgainInANewLoad) {
  EchoVR::CJsonGetFloat = &GameCJsonGetFloat;
  StageAndArm();
  SetLoadedLevels({kSub});
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  PVOID root = nevr_config_alias_hook::LookupDetour(kArenaRulesKey);
  g_arenaRoundTime = 240.0f;
  EXPECT_EQ(CJsonGetFloatHook(root, "round_time", 0.0f, 0), 240.0f);
  StageAndArm();  // the next load: a new generation of the alias table
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  root = nevr_config_alias_hook::LookupDetour(kArenaRulesKey);
  EXPECT_EQ(CJsonGetFloatHook(root, "round_time", 0.0f, 0), 240.0f);
  EXPECT_EQ(LinesWith("config_alias override key=round_time map=m1"), 2u);
  g_arenaRoundTime = 0.0f;
}

// The same mount, no Stage in between: the sublevel unloads and loads again, the alias goes live again, and the override is
// logged again. Red before the epoch keying: the dedupe was on the table's generation, which a reload does not move.
TEST_F(ConfigAliasHookTest, AReloadedSublevelLogsTheOverrideAgainWithoutAStage) {
  EchoVR::CJsonGetFloat = &GameCJsonGetFloat;
  StageAndArm();
  SetLoadedLevels({kSub});
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  PVOID root = nevr_config_alias_hook::LookupDetour(kArenaRulesKey);
  g_arenaRoundTime = 240.0f;
  EXPECT_EQ(CJsonGetFloatHook(root, "round_time", 0.0f, 0), 240.0f);
  SetLoadedLevels({});
  nevr_config_alias_hook::ReconcileIfFrameMoved();  // the alias is not live
  SetLoadedLevels({kSub});
  nevr_config_alias_hook::ReconcileIfFrameMoved();  // live again, no Stage, no Arm
  root = nevr_config_alias_hook::LookupDetour(kArenaRulesKey);
  ASSERT_EQ(root, &g_aliasRules);
  EXPECT_EQ(CJsonGetFloatHook(root, "round_time", 0.0f, 0), 240.0f);
  EXPECT_EQ(LinesWith("config_alias override key=round_time map=m1"), 2u);
  g_arenaRoundTime = 0.0f;
}

// A read of another record under the same path while the alias is live returns config.yaml's value, and is not reported as
// the map's: the line is written only for the aliased record the lookup answered.
TEST_F(ConfigAliasHookTest, AReadOfAnotherRecordWhileTheAliasIsLiveIsNotReportedAsTheMaps) {
  EchoVR::CJsonGetFloat = &GameCJsonGetFloat;
  StageAndArm();
  SetLoadedLevels({kSub});
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  ASSERT_EQ(nevr_config_alias_hook::LookupDetour(kArenaRulesKey), &g_aliasRules);
  g_arenaRoundTime = 240.0f;
  EXPECT_EQ(CJsonGetFloatHook(&g_stockRules, "round_time", 0.0f, 0), 240.0f);
  EXPECT_EQ(LinesWith("config_alias override"), 0u);
  g_arenaRoundTime = 0.0f;
}

// With no alias live the override still applies config.yaml's value, and nothing is logged about a map.
TEST_F(ConfigAliasHookTest, WithNoAliasLiveTheOverrideIsTheConfigValueAndNoLine) {
  EchoVR::CJsonGetFloat = &GameCJsonGetFloat;
  g_arenaRoundTime = 240.0f;
  EXPECT_EQ(CJsonGetFloatHook(&g_stockRules, "round_time", 0.0f, 0), 240.0f);
  EXPECT_TRUE(g_lines.empty());
  StageAndArm();
  SetLoadedLevels({});  // staged and armed, but no level of the map is loaded: the alias is not live
  nevr_config_alias_hook::ReconcileIfFrameMoved();
  EXPECT_EQ(CJsonGetFloatHook(&g_stockRules, "round_time", 0.0f, 0), 240.0f);
  EXPECT_EQ(LinesWith("config_alias override"), 0u);
  g_arenaRoundTime = 0.0f;
}

// The offsets and addresses the hook reads, pinned to the values ReVault reads from echovr.exe (sha256 b6d08277...). A
// changed value here is a changed binary, and the test refuses it before the hook would read the wrong bytes.
TEST(ConfigAliasHookAddresses, TheReadAddressesAreTheOnesReVaultReadsInTheBinary) {
  namespace addr = nevr_patch_addresses;
  EXPECT_EQ(addr::CONFIG_BALANCE_LOOKUP, 0xD0B170u);
  EXPECT_EQ(addr::CGAMELEVELRESOURCE_ENABLE, 0x4ED4B0u);
  EXPECT_EQ(addr::CGAMELEVELRESOURCE_DISABLE, 0x4EC900u);
  EXPECT_EQ(addr::NETGAME_BALANCE_SETTINGS_UPDATE, 0x1B2A90u);
  EXPECT_EQ(addr::GAME_SPACE_TABLE_OFFSET, 0x1F90u);
  EXPECT_EQ(addr::GAME_SPACE_TABLE_COUNT_OFFSET, 0x1FC0u);
  EXPECT_EQ(addr::GAME_ENABLED_SPACE_TABLE_OFFSET, 0x1F50u);
  EXPECT_EQ(addr::GAME_ENABLED_SPACE_TABLE_COUNT_OFFSET, 0x1F80u);
  EXPECT_EQ(addr::LEVEL_RESOURCE_LEVEL_OFFSET, 0x58u);
  EXPECT_EQ(addr::LEVEL_RESOURCE_FLAGS_OFFSET, 0xC40u);
  EXPECT_EQ(addr::LEVEL_RESOURCE_DISABLED_FLAG, 0x80u);
  EXPECT_EQ(addr::NETGAME_GAME_OFFSET, 8u);
}

}  // namespace
