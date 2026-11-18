// The PC map channel's own decisions (content/map_channel_rules.h): the owners a mount grants, what the mount observer takes, what a
// server's group fetch does with each outcome, how the server's worker loops, and when the boot refuses a run. Fakes only: no Win32,
// no game, no cache.
#include "runtime/content/map_channel_rules.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace nevr_map_channel::pc;

constexpr std::uint32_t kWaitMs = 20;
constexpr std::uint64_t kLevelA = 0x1A;
constexpr std::uint64_t kLevelB = 0xBEEF;
constexpr std::size_t kTwo = 2;
constexpr std::size_t kMaxChecks = 8;  // a bound so a loop that fetches nothing ends the test instead of hanging it

nevr_maps::MountReport Mounted(std::vector<std::string> ids) {
  nevr_maps::MountReport report;
  report.mounted = std::move(ids);
  return report;
}

nevr_maps::FetchReport Fetch(nevr_maps::FetchOutcome outcome, const std::string& reason = "") {
  nevr_maps::FetchReport report;
  report.group = "g";
  report.outcome = outcome;
  report.reason = reason;
  return report;
}

TEST(GrantedOwners, NamesOnlyTheMountedMapsWithTheOwnerPrefix) {
  nevr_maps::MountReport report = Mounted({"arena_a", "arena_b"});
  report.kept = {"arena_kept"};
  report.deferred = {"arena_deferred"};
  report.skipped = {"arena_skipped"};
  report.refused = {{"arena_refused", "bad"}};
  EXPECT_EQ(GrantedOwners(report), (std::vector<std::string>{"map:arena_a", "map:arena_b"}));
  EXPECT_TRUE(GrantedOwners(nevr_maps::MountReport{}).empty());
}

TEST(MountedOwners, ReadsTheMapOwnersAndTheirHexLevels) {
  const std::string report = R"([
    {"plugin":"map:arena_a","levels":["1a","BEEF"]},
    {"plugin":"some_plugin","levels":["ff"]},
    {"plugin":"map:arena_b"},
    {"plugin":"map:arena_c","levels":["zz",5,"2"]}
  ])";
  const nevr_config_alias::OwnerLevels owners = MountedOwners(report);
  ASSERT_EQ(owners.size(), 3u);
  EXPECT_EQ(owners[0].first, "map:arena_a");
  EXPECT_EQ(owners[0].second, (std::vector<std::uint64_t>{kLevelA, kLevelB}));
  EXPECT_EQ(owners[1].first, "map:arena_b");
  EXPECT_TRUE(owners[1].second.empty());
  EXPECT_EQ(owners[2].first, "map:arena_c");
  EXPECT_EQ(owners[2].second, (std::vector<std::uint64_t>{0, kTwo}));  // a non-hex string reads 0; a non-string level is skipped
}

TEST(MountedOwners, AReportThatIsNotAnArrayOfSetsNamesNoOwner) {
  EXPECT_TRUE(MountedOwners("").empty());
  EXPECT_TRUE(MountedOwners("not json").empty());
  EXPECT_TRUE(MountedOwners(R"({"plugin":"map:a"})").empty());
  EXPECT_TRUE(MountedOwners(R"(["map:a", 3, null, {"plugin":7}, {"nothing":1}])").empty());
}

TEST(GrantLedger, TakeHandsOverEveryGrantAndHoldOnceAndEmptiesTheLedger) {
  GrantLedger ledger;
  ledger.Grant(Mounted({"a"}));
  ledger.Grant(Mounted({"b", "c"}));
  ledger.Hold("g1", Mounted({"a"}));
  GrantLedger::Taken first = ledger.Take();
  EXPECT_EQ(first.granted, (std::vector<std::string>{"map:a", "map:b", "map:c"}));
  ASSERT_EQ(first.holds.size(), 1u);
  EXPECT_EQ(first.holds[0].first, "g1");
  EXPECT_EQ(first.holds[0].second.mounted, std::vector<std::string>{"a"});
  const GrantLedger::Taken second = ledger.Take();
  EXPECT_TRUE(second.granted.empty());
  EXPECT_TRUE(second.holds.empty());
  ledger.Grant(Mounted({"d"}));
  EXPECT_EQ(ledger.Take().granted, std::vector<std::string>{"map:d"});
}

TEST(FinishServerGroup, AFailedFetchTellsTheHoldAndMountsNothing) {
  GrantLedger ledger;
  std::vector<std::string> failed;
  int mounts = 0;
  int installs = 0;
  const nevr_maps::FetchReport report = Fetch(nevr_maps::FetchOutcome::kFailed, "manifest_unreachable");
  const nevr_maps::FetchReport out = FinishServerGroup(
      "g", report, ledger, [&](const std::string& g) { failed.push_back(g); },
      [&](const std::string&) {
        ++mounts;
        return nevr_maps::MountReport{};
      },
      [&](const nevr_maps::MountReport&) { ++installs; });
  EXPECT_EQ(failed, std::vector<std::string>{"g"});
  EXPECT_EQ(mounts, 0);
  EXPECT_EQ(installs, 0);
  EXPECT_TRUE(ledger.Take().holds.empty());
  EXPECT_EQ(out.outcome, nevr_maps::FetchOutcome::kFailed);
  EXPECT_EQ(out.reason, "manifest_unreachable");
}

TEST(FinishServerGroup, AFetchedOrCachedGroupMountsRecordsTheHoldThenInstalls) {
  for (const nevr_maps::FetchOutcome outcome : {nevr_maps::FetchOutcome::kFetched, nevr_maps::FetchOutcome::kCached}) {
    GrantLedger ledger;
    std::vector<std::string> order;
    std::vector<std::string> failed;
    const nevr_maps::FetchReport out = FinishServerGroup(
        "g", Fetch(outcome), ledger, [&](const std::string& g) { failed.push_back(g); },
        [&](const std::string& g) {
          order.push_back("mount " + g);
          return Mounted({"arena_a"});
        },
        [&](const nevr_maps::MountReport& m) {
          // The hold is recorded before the install runs.
          order.push_back("install " + m.mounted.front() + " holds=" + std::to_string(ledger.Take().holds.size()));
        });
    EXPECT_TRUE(failed.empty());
    EXPECT_EQ(order, (std::vector<std::string>{"mount g", "install arena_a holds=1"}));
    EXPECT_EQ(out.outcome, outcome);
  }
}

TEST(FinishServerGroup, TheHoldCarriesTheMountReportForTheObserver) {
  GrantLedger ledger;
  FinishServerGroup(
      "g", Fetch(nevr_maps::FetchOutcome::kFetched), ledger, [](const std::string&) {},
      [](const std::string&) { return Mounted({"arena_a", "arena_b"}); }, [](const nevr_maps::MountReport&) {});
  const GrantLedger::Taken taken = ledger.Take();
  ASSERT_EQ(taken.holds.size(), 1u);
  EXPECT_EQ(taken.holds[0].first, "g");
  EXPECT_EQ(taken.holds[0].second.mounted, (std::vector<std::string>{"arena_a", "arena_b"}));
}

TEST(RunServerPass, RunsEachRequestedGroupInOrderAndStopsWhenKeepGoingTurnsFalse) {
  nevr_maps::GroupFetchQueue queue;
  std::vector<std::string> done;
  std::string reason;
  ASSERT_TRUE(queue.Request("aaaa", [&](const nevr_maps::FetchReport& r) { done.push_back("done " + r.group); }, &reason));
  ASSERT_TRUE(queue.Request("bbbb", [&](const nevr_maps::FetchReport& r) { done.push_back("done " + r.group); }, &reason));
  std::vector<std::string> fetched;
  std::size_t checks = 0;
  RunServerPass(
      queue,
      [&] {
        ++checks;
        return checks < kMaxChecks && (queue.Waiting() != 0 || checks == 1);  // the loop checks again after each group
      },
      kWaitMs,
      [&](const std::string& group) {
        fetched.push_back(group);
        nevr_maps::FetchReport report;
        report.group = group;
        return report;
      });
  EXPECT_EQ(fetched, (std::vector<std::string>{"aaaa", "bbbb"}));
  EXPECT_EQ(done, (std::vector<std::string>{"done aaaa", "done bbbb"}));
}

TEST(RunServerPass, AnIdleQueueFetchesNothingAndStillChecksForAStop) {
  nevr_maps::GroupFetchQueue queue;
  int fetches = 0;
  int checks = 0;
  RunServerPass(
      queue, [&] { return ++checks <= static_cast<int>(kTwo); }, kWaitMs,
      [&](const std::string&) {
        ++fetches;
        return nevr_maps::FetchReport{};
      });
  EXPECT_EQ(fetches, 0);
  EXPECT_EQ(checks, static_cast<int>(kTwo) + 1);
}

TEST(PreflightRefusal, OnlyARunTheCoreDecidedToStartCanBeRefused) {
  bool asked = false;
  const auto supported = [&] {
    asked = true;
    return false;
  };
  for (const nevr_map_channel::Start start : {nevr_map_channel::Start::kOff, nevr_map_channel::Start::kRefused}) {
    EXPECT_EQ(PreflightRefusal(start, supported, /*root_empty=*/true), BootRefusal::kNone);
  }
  EXPECT_FALSE(asked);  // the game build is not even looked at
}

TEST(PreflightRefusal, AnUnsupportedGameBuildIsRefusedBeforeAMissingRoot) {
  EXPECT_EQ(PreflightRefusal(nevr_map_channel::Start::kRun, [] { return false; }, /*root_empty=*/true),
            BootRefusal::kUnsupportedGameBuild);
  EXPECT_EQ(PreflightRefusal(nevr_map_channel::Start::kRun, [] { return false; }, /*root_empty=*/false),
            BootRefusal::kUnsupportedGameBuild);
}

TEST(PreflightRefusal, ASupportedBuildWithNoRootIsRefusedAndOtherwiseTheRunGoesOn) {
  EXPECT_EQ(PreflightRefusal(nevr_map_channel::Start::kRun, [] { return true; }, /*root_empty=*/true), BootRefusal::kNoRoot);
  EXPECT_EQ(PreflightRefusal(nevr_map_channel::Start::kRun, [] { return true; }, /*root_empty=*/false), BootRefusal::kNone);
}

}  // namespace
