// Same-owner replace of a mounted map (core/map_swap.h), on the Test edge's Env: swap, defer and restart, the old keys never served
// after a swap, and the other maps untouched.
#include "core/map_swap.h"

#include <gtest/gtest.h>

#include "quest/tests/cdn_fake_env.h"

namespace {

using nevr_maps::AssetKey;
using nevr_maps::MapSet;
using nevr_maps::MapSwaps;
using nevr_maps::SwapStep;
using nevr_maps::SwapView;

constexpr std::uint64_t kTexture = 0xbeac1969cb7b8861ull;
const AssetKey kShared{kTexture, 1};   // in both versions of arena_c
const AssetKey kOldOnly{kTexture, 2};  // only in version 1
const AssetKey kNewOnly{kTexture, 3};  // only in version 2
const AssetKey kOther{kTexture, 9};    // arena_d's

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
  EXPECT_EQ(nevr_maps::DecideSwap(SwapView{true, 3}), SwapStep::kDefer);
  EXPECT_EQ(nevr_maps::DecideSwap(SwapView{false, 1}), SwapStep::kRestart);
  EXPECT_EQ(nevr_maps::DecideSwap(SwapView{false, 0}), SwapStep::kSwap);
}

TEST_F(MapSwapTest, NothingLoadedSwapsTheNewSetServesTheMapsKeysAndTheOldSetServesNothing) {
  EXPECT_EQ(StepWith(SwapView{}), std::vector<std::string>{"arena_c"});
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], std::make_pair(std::string("arena_c"), std::string("1")));  // the engine is asked about the old set's keys
  ASSERT_NE(swaps.MountedSet("arena_c"), nullptr);
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "2");
  EXPECT_EQ(swaps.ServedBy(kShared), "arena_c");
  EXPECT_EQ(swaps.ServedBy(kNewOnly), "arena_c");
  EXPECT_EQ(swaps.ServedBy(kOldOnly), "");  // a key only the old version had is served by no map
  EXPECT_EQ(swaps.ServedBy(kOther), "arena_d");
  EXPECT_EQ(swaps.MountedSet("arena_d")->version, "1");
  EXPECT_FALSE(swaps.Waiting("arena_c"));
  const cdn_test::Record* swapped = env.FindRecord("map_swap", "decision", "swap");
  ASSERT_NE(swapped, nullptr);
  EXPECT_EQ(swapped->Get("from"), "1");
  EXPECT_EQ(swapped->Get("to"), "2");
  EXPECT_TRUE(StepWith(SwapView{}).empty());  // done once
}

TEST_F(MapSwapTest, ALoadedLevelDefersTheSwapUntilItUnloads) {
  EXPECT_TRUE(StepWith(SwapView{true, 4}).empty());
  EXPECT_TRUE(StepWith(SwapView{true, 4}).empty());
  EXPECT_TRUE(swaps.Waiting("arena_c"));
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "1");
  EXPECT_EQ(swaps.ServedBy(kOldOnly), "arena_c");  // the loaded level keeps reading its own version
  EXPECT_EQ(swaps.ServedBy(kNewOnly), "");
  EXPECT_EQ(Records("defer"), 1u);
  EXPECT_EQ(StepWith(SwapView{false, 0}), std::vector<std::string>{"arena_c"});
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "2");
  EXPECT_EQ(swaps.ServedBy(kOldOnly), "");
}

TEST_F(MapSwapTest, AKeyStillLoadedAfterTheUnloadRefusesWithTheRestartNoticeAndTheOldSetStays) {
  EXPECT_TRUE(StepWith(SwapView{false, 2}).empty());
  EXPECT_FALSE(swaps.Waiting("arena_c"));
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "1");
  EXPECT_EQ(swaps.ServedBy(kOldOnly), "arena_c");
  EXPECT_EQ(swaps.ServedBy(kNewOnly), "");
  EXPECT_EQ(swaps.RestartNotices(), std::vector<std::string>{"Restart the game to play the update of arena_c."});
  const cdn_test::Record* refused = env.FindRecord("map_swap", "decision", "restart");
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(refused->Get("keys_loaded"), "2");
  EXPECT_EQ(refused->level, nevr_cdn::EventLevel::kWarn);
  EXPECT_TRUE(StepWith(SwapView{}).empty());  // refused for the run: a later step does not swap it in
  EXPECT_EQ(swaps.MountedSet("arena_c")->version, "1");
}

TEST_F(MapSwapTest, OnlyAMountedMapAtAnotherVersionWaits) {
  EXPECT_FALSE(swaps.Stored("arena_e", MapSet{"1", {}}));
  EXPECT_FALSE(swaps.Stored("arena_d", MapSet{"1", {kOther}}));
  EXPECT_FALSE(swaps.Waiting("arena_d"));
  EXPECT_TRUE(swaps.Waiting("arena_c"));
}

}  // namespace
