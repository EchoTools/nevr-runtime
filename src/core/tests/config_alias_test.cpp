// ConfigAlias's pure half (core/config_alias.h; docs/adr/0048, decision 5): staging, arming, the winner rule, the
// interned snapshots, and the revalidated sublevel set. Each test builds its own Table, except the allocation test,
// which reads the process's one. Log() is captured through the boot sink.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "core/config_alias.h"
#include "core/logging.h"

namespace {

std::atomic<bool> g_countAllocations{false};
std::atomic<int> g_allocations{0};

// Behind noinline helpers so an optimising build cannot inline `free` into a caller that inlined `operator new`
// (-Wmismatched-new-delete), as in quest/redirect/tests/redirect_test.cpp.
__attribute__((noinline)) void* RawAllocate(std::size_t size) { return std::malloc(size == 0 ? 1 : size); }
__attribute__((noinline)) void RawFree(void* p) { std::free(p); }

}  // namespace

void* operator new(std::size_t size) {
  if (g_countAllocations.load()) g_allocations.fetch_add(1);
  if (void* p = RawAllocate(size)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { RawFree(p); }
void operator delete(void* p, std::size_t) noexcept { RawFree(p); }

namespace {

using nevr_config_alias::kArenaRulesKey;
using nevr_config_alias::OwnerLevels;
using nevr_config_alias::Pair;
using nevr_config_alias::Table;
using nevr_config_alias::Winner;

std::vector<std::string> g_lines;

VOID CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_lines.emplace_back(message); }

class ConfigAliasTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_lines.clear();
    SetBootLogSink(&CaptureLog);
  }
  void TearDown() override { SetBootLogSink(nullptr); }

  std::size_t LinesWith(const std::string& text) const {
    std::size_t n = 0;
    for (const std::string& line : g_lines) n += line.find(text) != std::string::npos ? 1 : 0;
    return n;
  }
};

// The engine's CRC-64 (polynomial 0x95AC9329AC4BC9B5, lower-cased, sign-extended input), recomputed here so the
// pinned key is checked against its name.
std::uint64_t RadHash(const std::string& text) {
  std::uint64_t table[256];
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint64_t r = 0;
    for (int bit = 7; bit >= 0; --bit) {
      if ((i >> bit) & 1u) r ^= 0x95AC9329AC4BC9B5ull;
      r <<= 1;
    }
    table[i] = r;
  }
  std::uint64_t h = ~0ull;
  for (const char raw : text) {
    auto c = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<signed char>(raw)));
    if (raw >= 'A' && raw <= 'Z') c += 0x20;
    h = (c ^ table[(h >> 56) & 0xFF]) ^ (h << 8);
  }
  return h;
}

bool HoldsAll(void*, const std::string&, std::uint64_t) { return true; }
bool HoldsNone(void*, const std::string&, std::uint64_t) { return false; }

constexpr std::uint64_t kAliasA = 0xA1, kAliasB = 0xB1;
constexpr std::uint64_t kMainA = 0x1000, kSubA = 0x1001, kMainB = 0x2000, kSubB = 0x2001, kStock = 0x9000;

// Map "alpha" (owner "oa") and map "beta" (owner "ob"), each with a main level and one sublevel, staged and armed.
void StageAndArmBoth(Table& t) {
  ASSERT_EQ(t.Stage("oa", "alpha", kMainA, {{kArenaRulesKey, kAliasA}}), 1u);
  ASSERT_EQ(t.Stage("ob", "beta", kMainB, {{kArenaRulesKey, kAliasB}}), 1u);
  const OwnerLevels mounted = {{"oa", {kMainA, kSubA}}, {"ob", {kMainB, kSubB}}};
  ASSERT_EQ(t.Arm(mounted, &HoldsAll, nullptr), 2u);
}

TEST_F(ConfigAliasTest, TheAllowedKeyIsArenaRulesOnly) {
  EXPECT_EQ(kArenaRulesKey, RadHash("mp_arena_rules"));
  EXPECT_TRUE(nevr_config_alias::KeyAllowed(kArenaRulesKey));
  EXPECT_FALSE(nevr_config_alias::KeyAllowed(RadHash("mp_shared_movement")));
  EXPECT_FALSE(nevr_config_alias::KeyAllowed(0));
}

TEST_F(ConfigAliasTest, StageKeepsOnlyAllowedDistinctPairsUpToTheCap) {
  Table t;
  EXPECT_EQ(t.Stage("o", "m", kMainA, {{RadHash("mp_shared_movement"), kAliasA}}), 0u);
  EXPECT_EQ(t.Stage("o", "m", kMainA, {{kArenaRulesKey, kArenaRulesKey}}), 0u);
  EXPECT_EQ(t.Stage("o", "m", kMainA, {{kArenaRulesKey, 0}}), 0u);
  EXPECT_FALSE(t.AnyStaged());
  const std::vector<Pair> five(5, Pair{kArenaRulesKey, kAliasA});
  EXPECT_EQ(t.Stage("o", "m", kMainA, five), 1u);
  EXPECT_LE(t.Stage("o", "m", kMainA, five), nevr_config_alias::kMaxPerMap);
  EXPECT_TRUE(t.AnyStaged());
}

TEST_F(ConfigAliasTest, ResolveIsTheIdentityBeforeArm) {
  Table t;
  ASSERT_EQ(t.Stage("oa", "alpha", kMainA, {{kArenaRulesKey, kAliasA}}), 1u);
  t.Reconcile(kMainA, {kMainA, kSubA});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kArenaRulesKey);
  EXPECT_TRUE(t.Decide(kMainA, {kMainA}).empty());
}

TEST_F(ConfigAliasTest, ASublevelAloneActivatesTheAlias) {
  Table t;
  StageAndArmBoth(t);
  t.Reconcile(kStock, {kStock, kSubA});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kAliasA);
  EXPECT_EQ(t.Resolve(0x1234), 0x1234u);
}

TEST_F(ConfigAliasTest, TheMainLevelCountsAsLoaded) {
  Table t;
  StageAndArmBoth(t);
  t.Reconcile(kMainB, {});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kAliasB);
}

TEST_F(ConfigAliasTest, UnloadingTheLastLevelRestoresTheIdentity) {
  Table t;
  StageAndArmBoth(t);
  t.Reconcile(kStock, {kStock, kSubA});
  ASSERT_EQ(t.Resolve(kArenaRulesKey), kAliasA);
  t.Reconcile(kStock, {kStock});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kArenaRulesKey);
  t.Reconcile(0, {});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kArenaRulesKey);
}

TEST_F(ConfigAliasTest, AFailedUnloadKeepsTheAlias) {
  Table t;
  StageAndArmBoth(t);
  t.Reconcile(kStock, {kStock, kSubA});
  t.Reconcile(kStock, {kStock, kSubA});  // the unload failed: the source still lists the sublevel
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kAliasA);
}

TEST_F(ConfigAliasTest, LoadOrderDoesNotChangeTheWinner) {
  Table first, second;
  StageAndArmBoth(first);
  StageAndArmBoth(second);
  first.Reconcile(kStock, {kSubA});
  first.Reconcile(kStock, {kSubA, kSubB});
  second.Reconcile(kStock, {kSubB});
  second.Reconcile(kStock, {kSubB, kSubA});
  EXPECT_EQ(first.Resolve(kArenaRulesKey), kAliasA);
  EXPECT_EQ(second.Resolve(kArenaRulesKey), kAliasA);
  EXPECT_EQ(first.Decide(kStock, {kSubB, kSubA}), second.Decide(kStock, {kSubA, kSubB}));
}

TEST_F(ConfigAliasTest, TheMainLevelsMapBeatsASublevelsMap) {
  Table t;
  StageAndArmBoth(t);
  t.Reconcile(kMainB, {kMainB, kSubA});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kAliasB);
  const std::vector<Winner> winners = t.Decide(kMainB, {kSubA});
  ASSERT_EQ(winners.size(), 1u);
  EXPECT_EQ(winners[0].owner, "ob");
  EXPECT_EQ(LinesWith("config_alias conflict key=mp_arena_rules winner=beta loser=alpha"), 1u);
}

TEST_F(ConfigAliasTest, TheLowestIdWinsAndTheLoserIsLoggedOnce) {
  Table t;
  StageAndArmBoth(t);
  t.Reconcile(kStock, {kSubB, kSubA});
  t.Reconcile(kStock, {kSubB});
  t.Reconcile(kStock, {kSubA, kSubB});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kAliasA);
  EXPECT_EQ(LinesWith("[NEVR.MAPS] config_alias conflict key=mp_arena_rules winner=alpha loser=beta"), 1u);
  EXPECT_EQ(LinesWith("config_alias conflict"), 1u);
}

TEST_F(ConfigAliasTest, AnOwnerWhoseSetDidNotMountIsNeverArmed) {
  Table t;
  ASSERT_EQ(t.Stage("oa", "alpha", kMainA, {{kArenaRulesKey, kAliasA}}), 1u);
  ASSERT_EQ(t.Stage("ob", "beta", kMainB, {{kArenaRulesKey, kAliasB}}), 1u);
  EXPECT_EQ(t.Arm({{"ob", {kMainB}}}, &HoldsAll, nullptr), 1u);
  t.Reconcile(kMainA, {kMainA, kSubA});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kArenaRulesKey);
}

TEST_F(ConfigAliasTest, AnAliasTheSetDoesNotHoldIsRefusedOnce) {
  Table t;
  ASSERT_EQ(t.Stage("oa", "alpha", kMainA, {{kArenaRulesKey, kAliasA}}), 1u);
  EXPECT_EQ(t.Arm({{"oa", {kMainA}}}, &HoldsNone, nullptr), 0u);
  EXPECT_EQ(t.Arm({{"oa", {kMainA}}}, &HoldsNone, nullptr), 0u);
  t.Reconcile(kMainA, {kMainA});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kArenaRulesKey);
  EXPECT_EQ(LinesWith("config_alias refused=alias_missing map=alpha key=mp_arena_rules"), 1u);
}

TEST_F(ConfigAliasTest, UnstagingTheWinnerDropsItsAliasAtOnce) {
  Table t;
  StageAndArmBoth(t);
  t.Reconcile(kStock, {kSubA});
  ASSERT_EQ(t.Resolve(kArenaRulesKey), kAliasA);
  const std::uint64_t before = t.Generation();
  t.Unstage("oa");
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kArenaRulesKey);
  EXPECT_GT(t.Generation(), before);
  t.Reconcile(kStock, {kSubA, kSubB});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kAliasB);
}

TEST_F(ConfigAliasTest, RestagingAnArmedOwnerDisarmsIt) {
  Table t;
  StageAndArmBoth(t);
  t.Reconcile(kStock, {kSubA});
  ASSERT_EQ(t.Resolve(kArenaRulesKey), kAliasA);
  ASSERT_EQ(t.Stage("oa", "alpha", kMainA, {{kArenaRulesKey, kAliasA}}), 1u);
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kArenaRulesKey);
  t.Reconcile(kStock, {kSubA});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kArenaRulesKey);
}

TEST_F(ConfigAliasTest, PastTheSnapshotCapANewCombinationReadsStock) {
  Table t;
  constexpr std::size_t kMaps = nevr_config_alias::kMaxSnapshots + 1;
  OwnerLevels mounted;
  for (std::size_t i = 0; i < kMaps; ++i) {
    const std::string id = "m" + std::to_string(100 + i);
    ASSERT_EQ(t.Stage("o" + id, id, 0x5000 + i, {{kArenaRulesKey, 0x7000 + i}}), 1u);
    mounted.push_back({"o" + id, {0x5000 + i}});
  }
  ASSERT_EQ(t.Arm(mounted, &HoldsAll, nullptr), kMaps);
  for (std::size_t i = 0; i < nevr_config_alias::kMaxSnapshots; ++i) {
    t.Reconcile(0x5000 + i, {});
    ASSERT_EQ(t.Resolve(kArenaRulesKey), 0x7000 + i);
  }
  t.Reconcile(0x5000 + nevr_config_alias::kMaxSnapshots, {});
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kArenaRulesKey);
  EXPECT_EQ(LinesWith("config_alias reason=snapshot_cap"), 1u);
  t.Reconcile(0x5000, {});  // an interned combination is still served
  EXPECT_EQ(t.Resolve(kArenaRulesKey), 0x7000u);
  t.Reconcile(0x5000 + nevr_config_alias::kMaxSnapshots, {});
  EXPECT_EQ(LinesWith("config_alias reason=snapshot_cap"), 1u);
}

TEST_F(ConfigAliasTest, ResolveAllocatesNothing) {
  Table& t = nevr_config_alias::Global();
  ASSERT_EQ(nevr_config_alias::Stage("alloc", "alloc", 0x6000, {{kArenaRulesKey, 0x6001}}), 1u);
  ASSERT_EQ(nevr_config_alias::Arm({{"alloc", {0x6000}}}, &HoldsAll, nullptr), 1u);
  nevr_config_alias::Reconcile(0x6000, {});
  g_allocations = 0;
  g_countAllocations = true;
  std::uint64_t answers = 0;
  for (int i = 0; i < 1000; ++i) answers += nevr_config_alias::Resolve(kArenaRulesKey) == 0x6001 ? 1 : 0;
  answers += nevr_config_alias::Resolve(0x42) == 0x42 ? 1 : 0;
  g_countAllocations = false;
  EXPECT_EQ(answers, 1001u);
  EXPECT_EQ(g_allocations.load(), 0);
  t.Unstage("alloc");
  EXPECT_EQ(nevr_config_alias::Resolve(kArenaRulesKey), kArenaRulesKey);
}

struct LiveSpaces {
  std::set<const void*> instances;
};

bool InLiveSpaces(void* ctx, const void* instance, std::uint64_t) {
  return static_cast<LiveSpaces*>(ctx)->instances.count(instance) != 0;
}

TEST(LevelInstancesTest, ATeardownWithoutDisableDropsTheLevelAtTheNextRead) {
  nevr_config_alias::LevelInstances source;
  int a = 0, b = 0;
  LiveSpaces live{{&a, &b}};
  source.OnEnabled(&a, kSubA);
  source.OnEnabled(&b, kSubB);
  EXPECT_EQ(source.Revalidate(1, kMainA, &InLiveSpaces, &live), (std::vector<std::uint64_t>{kSubA, kSubB}));
  live.instances.erase(&b);  // torn down, no Disable
  EXPECT_EQ(source.Revalidate(1, kMainA, &InLiveSpaces, &live), (std::vector<std::uint64_t>{kSubA}));
  live.instances.insert(&b);  // the address reused: an event alone never brings an entry back
  EXPECT_EQ(source.Revalidate(1, kMainA, &InLiveSpaces, &live), (std::vector<std::uint64_t>{kSubA}));
  source.OnDisabled(&a);
  EXPECT_TRUE(source.Revalidate(1, kMainA, &InLiveSpaces, &live).empty());
}

TEST(LevelInstancesTest, ASessionLeaveOrAMainLevelChangeClearsTheSet) {
  nevr_config_alias::LevelInstances source;
  int a = 0;
  LiveSpaces live{{&a}};
  source.OnEnabled(&a, kSubA);
  ASSERT_EQ(source.Revalidate(1, kMainA, &InLiveSpaces, &live).size(), 1u);
  EXPECT_TRUE(source.Revalidate(2, kMainA, &InLiveSpaces, &live).empty());
  source.OnEnabled(&a, kSubA);
  ASSERT_EQ(source.Revalidate(2, kMainA, &InLiveSpaces, &live).size(), 1u);
  EXPECT_TRUE(source.Revalidate(2, kMainB, &InLiveSpaces, &live).empty());
  EXPECT_EQ(source.size(), 0u);
}

// The arena override's log names the map: LiveMapId answers the winning map while its alias is published, and nothing
// otherwise.
TEST_F(ConfigAliasTest, LiveMapIdNamesTheMapWhoseAliasIsPublished) {
  Table t;
  EXPECT_EQ(t.LiveMapId(kArenaRulesKey), "");
  StageAndArmBoth(t);
  EXPECT_EQ(t.LiveMapId(kArenaRulesKey), "");  // armed, not yet published
  t.Reconcile(kMainB, {kSubA});
  EXPECT_EQ(t.LiveMapId(kArenaRulesKey), "beta");
  EXPECT_EQ(t.LiveMapId(0x1234), "");
  t.Reconcile(kStock, {});
  EXPECT_EQ(t.LiveMapId(kArenaRulesKey), "");
}

// A second Arm with the same mount is no new load: the generation stays, and the alias is answered as before.
TEST_F(ConfigAliasTest, ArmingTheSameMountAgainChangesNothing) {
  Table t;
  StageAndArmBoth(t);
  t.Reconcile(kStock, {kSubA});
  const std::uint64_t generation = t.Generation();
  const OwnerLevels mounted = {{"oa", {kMainA, kSubA}}, {"ob", {kMainB, kSubB}}};
  EXPECT_EQ(t.Arm(mounted, &HoldsAll, nullptr), 2u);
  EXPECT_EQ(t.Generation(), generation);
  EXPECT_EQ(t.Resolve(kArenaRulesKey), kAliasA);
  EXPECT_EQ(t.LiveMapId(kArenaRulesKey), "alpha");
  // A changed mount is a new load: the generation moves.
  EXPECT_EQ(t.Arm({{"oa", {kMainA}}, {"ob", {kMainB, kSubB}}}, &HoldsAll, nullptr), 2u);
  EXPECT_NE(t.Generation(), generation);
}

}  // namespace
