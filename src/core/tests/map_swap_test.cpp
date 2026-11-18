// Same-owner replace of a mounted map (core/map_swap.h), on the shared fake Env: the decision, the swap, the deferral and the
// restart, the old keys never served after a swap, the other maps untouched, and the version order that refuses a rollback.
#include "core/map_swap.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "quest/tests/cdn_fake_env.h"

namespace {

using nevr_maps::AssetKey;
using nevr_maps::MapSet;
using nevr_maps::MapSwaps;
using nevr_maps::SwapStep;
using nevr_maps::SwapView;

constexpr std::uint64_t kTexture = 0xbeac1969cb7b8861ull;
constexpr std::uint64_t kSharedName = 1;   // in both versions of arena_c
constexpr std::uint64_t kOldOnlyName = 2;  // only in version 1
constexpr std::uint64_t kNewOnlyName = 3;  // only in version 2
constexpr std::uint64_t kOtherName = 9;    // arena_d's
const AssetKey kShared{kTexture, kSharedName};
const AssetKey kOldOnly{kTexture, kOldOnlyName};
const AssetKey kNewOnly{kTexture, kNewOnlyName};
const AssetKey kOther{kTexture, kOtherName};

constexpr std::size_t kKeysLoadedWhileLevelLoaded = 4;  // any count defers while a level is loaded
constexpr std::size_t kSomeKeysLoaded = 3;              // the decision table's count of loaded keys
constexpr std::size_t kStillLoadedKeys = 2;             // keys the engine still holds after the unload

class MapSwapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    swaps.Mounted("arena_c", MapSet{"1", {kShared, kOldOnly}});
    swaps.Mounted("arena_d", MapSet{"1", {kOther}});
    ASSERT_TRUE(swaps.Stored("arena_c", MapSet{"2", {kShared, kNewOnly}}));
  }
  std::vector<std::string> StepWith(SwapView view) {
    return swaps.Step([&](const std::string& id, const MapSet& mounted) {
      asked.emplace_back(id, mounted.version);
      return view;
    });
  }
  std::size_t Records(const std::string& decision) const {
    std::size_t n = 0;
    for (const cdn_test::Record& r : env.records)
      if (r.name == "map_swap" && r.Get("decision") == decision) ++n;
    return n;
  }

  cdn_test::FakeEnv env;
  MapSwaps swaps{env};
  std::vector<std::pair<std::string, std::string>> asked;  // (id, the mounted version the view was asked about)
};

TEST(MapSwapDecision, ALoadedLevelDefersAKeyLeftLoadedAfterTheUnloadRefusesNothingLoadedSwaps) {
  EXPECT_EQ(nevr_maps::DecideSwap(SwapView{true, 0}), SwapStep::kDefer);
  EXPECT_EQ(nevr_maps::DecideSwap(SwapView{true, kSomeKeysLoaded}), SwapStep::kDefer);
  EXPECT_EQ(nevr_maps::DecideSwap(SwapView{false, 1}), SwapStep::kRestart);
  EXPECT_EQ(nevr_maps::DecideSwap(SwapView{false, 0}), SwapStep::kSwap);
}

TEST_F(MapSwapTest, NothingLoadedSwapsTheNewSetServesTheMapsKeysAndTheOldSetServesNothing) {
  EXPECT_EQ(StepWith(SwapView{}), std::vector<std::string>{"arena_c"});
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], std::make_pair(std::string("arena_c"), std::string("1")));  // the engine is asked about the old set's keys
  ASSERT_NE(swaps.MountedSet("arena_c"), nullptr);
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "2");
  ASSERT_NE(swaps.ServedBy(kShared), nullptr);
  EXPECT_EQ(*swaps.ServedBy(kShared), "arena_c");
  ASSERT_NE(swaps.ServedBy(kNewOnly), nullptr);
  EXPECT_EQ(*swaps.ServedBy(kNewOnly), "arena_c");
  EXPECT_EQ(swaps.ServedBy(kOldOnly), nullptr);  // a key only the old version had is served by no map
  ASSERT_NE(swaps.ServedBy(kOther), nullptr);
  EXPECT_EQ(*swaps.ServedBy(kOther), "arena_d");
  EXPECT_EQ(swaps.MountedSet("arena_d")->version, "1");
  EXPECT_FALSE(swaps.Waiting("arena_c"));
  const cdn_test::Record* swapped = env.FindRecord("map_swap", "decision", "swap");
  ASSERT_NE(swapped, nullptr);
  EXPECT_EQ(swapped->Get("from"), "1");
  EXPECT_EQ(swapped->Get("to"), "2");
  EXPECT_TRUE(StepWith(SwapView{}).empty());  // done once
}

TEST_F(MapSwapTest, ALoadedLevelDefersTheSwapUntilItUnloads) {
  EXPECT_TRUE(StepWith(SwapView{true, kKeysLoadedWhileLevelLoaded}).empty());
  EXPECT_TRUE(StepWith(SwapView{true, kKeysLoadedWhileLevelLoaded}).empty());
  EXPECT_TRUE(swaps.Waiting("arena_c"));
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "1");
  ASSERT_NE(swaps.ServedBy(kOldOnly), nullptr);
  EXPECT_EQ(*swaps.ServedBy(kOldOnly), "arena_c");  // the loaded level keeps reading its own version
  EXPECT_EQ(swaps.ServedBy(kNewOnly), nullptr);
  EXPECT_EQ(Records("defer"), 1u);
  EXPECT_EQ(StepWith(SwapView{false, 0}), std::vector<std::string>{"arena_c"});
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "2");
  EXPECT_EQ(swaps.ServedBy(kOldOnly), nullptr);
}

TEST_F(MapSwapTest, AKeyStillLoadedAfterTheUnloadRefusesWithTheRestartNoticeAndTheOldSetStays) {
  EXPECT_TRUE(StepWith(SwapView{false, kStillLoadedKeys}).empty());
  EXPECT_FALSE(swaps.Waiting("arena_c"));
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "1");
  ASSERT_NE(swaps.ServedBy(kOldOnly), nullptr);
  EXPECT_EQ(*swaps.ServedBy(kOldOnly), "arena_c");
  EXPECT_EQ(swaps.ServedBy(kNewOnly), nullptr);
  EXPECT_EQ(swaps.RestartNotices(), std::vector<std::string>{"Restart the game to play the update of arena_c."});
  const cdn_test::Record* refused = env.FindRecord("map_swap", "decision", "restart");
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(refused->Get("keys_loaded"), std::to_string(kStillLoadedKeys));
  EXPECT_EQ(refused->level, nevr_cdn::EventLevel::kWarn);
  EXPECT_TRUE(StepWith(SwapView{}).empty());  // refused for the run: a later step does not swap it in
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "1");
}

TEST_F(MapSwapTest, OnlyAMountedMapAtANewerVersionWaits) {
  EXPECT_FALSE(swaps.Stored("arena_e", MapSet{"1", {}}));       // not mounted
  EXPECT_FALSE(swaps.Stored("arena_d", MapSet{"1", {kOther}}));  // the mounted version itself
  EXPECT_FALSE(swaps.Waiting("arena_d"));
  EXPECT_TRUE(swaps.Waiting("arena_c"));
}

TEST_F(MapSwapTest, AnOlderVersionIsRefusedAsARollback) {
  // arena_c is mounted at 1 and waits at 2 (SetUp). A rollback to 0 is refused, and the waiting 2 is not replaced by it.
  EXPECT_FALSE(swaps.Stored("arena_c", MapSet{"0", {kOldOnly}}));
  EXPECT_EQ(StepWith(SwapView{}), std::vector<std::string>{"arena_c"});
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "2");
}

TEST_F(MapSwapTest, VersionsAreComparedAsDottedNumbersNotAsText) {
  // "1.10" is newer than "1.9" as a number, and "1.9" is newer than "1.10" as text: the order is CompareVersions', ADR 0048 decision 3.
  MapSwaps numeric{env};
  numeric.Mounted("arena_f", MapSet{"1.9", {}});
  EXPECT_TRUE(numeric.Stored("arena_f", MapSet{"1.10", {}}));
  EXPECT_FALSE(numeric.Stored("arena_f", MapSet{"1.9", {}}));  // equal to the mounted version
}

TEST_F(MapSwapTest, ALowerNewerVersionDoesNotReplaceAWaitingOne) {
  MapSwaps waiting{env};
  waiting.Mounted("arena_g", MapSet{"1.0", {}});
  EXPECT_TRUE(waiting.Stored("arena_g", MapSet{"1.10", {}}));
  EXPECT_FALSE(waiting.Stored("arena_g", MapSet{"1.4", {}}));  // newer than mounted, older than the waiting 1.10
  waiting.Step([](const std::string&, const MapSet&) { return SwapView{}; });
  EXPECT_EQ(waiting.MountedSet("arena_g")->version, "1.10");
}

TEST_F(MapSwapTest, AnEqualVersionDoesNotReplaceAWaitingOne) {
  MapSwaps waiting{env};
  waiting.Mounted("arena_i", MapSet{"1.0", {}});
  EXPECT_TRUE(waiting.Stored("arena_i", MapSet{"1.10", {kOldOnly}}));
  EXPECT_FALSE(waiting.Stored("arena_i", MapSet{"1.10", {kNewOnly}}));  // the same version as the waiting one: first stays
  waiting.Step([](const std::string&, const MapSet&) { return SwapView{}; });
  EXPECT_NE(waiting.ServedBy(kOldOnly), nullptr);
  EXPECT_EQ(waiting.ServedBy(kNewOnly), nullptr);
}

TEST_F(MapSwapTest, ARollbackIsRefusedWhenNothingWaits) {
  MapSwaps rollback{env};
  rollback.Mounted("arena_j", MapSet{"1.5", {}});
  EXPECT_FALSE(rollback.Stored("arena_j", MapSet{"1.4", {}}));
  EXPECT_TRUE(rollback.Step([](const std::string&, const MapSet&) { return SwapView{}; }).empty());
  EXPECT_EQ(rollback.MountedSet("arena_j")->version, "1.5");
}

TEST_F(MapSwapTest, AKeyAnotherMapServesStaysThatMapsAcrossAnotherMapsSwap) {
  // kOther is arena_d's. arena_h also lists it, but the table keeps the first owner, so a swap of arena_h must not take it away.
  MapSwaps shared{env};
  shared.Mounted("arena_d", MapSet{"1", {kOther}});
  shared.Mounted("arena_h", MapSet{"1", {kOther}});
  ASSERT_TRUE(shared.Stored("arena_h", MapSet{"2", {}}));
  shared.Step([](const std::string&, const MapSet&) { return SwapView{}; });
  ASSERT_NE(shared.ServedBy(kOther), nullptr);
  EXPECT_EQ(*shared.ServedBy(kOther), "arena_d");
}

TEST_F(MapSwapTest, RemountingAMapStopsServingItsOldKeys) {
  swaps.Mounted("arena_d", MapSet{"2", {kNewOnly}});
  EXPECT_EQ(swaps.ServedBy(kOther), nullptr);
  ASSERT_NE(swaps.ServedBy(kNewOnly), nullptr);
  EXPECT_EQ(*swaps.ServedBy(kNewOnly), "arena_d");
  EXPECT_EQ(swaps.MountedSet("arena_d")->version, "2");
}

}  // namespace
