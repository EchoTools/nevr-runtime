// P-SUBLEVEL's A/B matching over recorded streams (src/runtime/maps/probe/probe_logic.cpp).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "runtime/maps/probe/probe_logic.h"

using nevr_maps_probe::DiffLevels;
using nevr_maps_probe::SublevelMatcher;

namespace {

constexpr std::uint64_t kLobby = 0x1111;
constexpr std::uint64_t kLobbyArena = 0x2222;
constexpr std::uint64_t kLobbyCombat = 0x3333;

bool AnyLineHas(const std::vector<std::string>& lines, const std::string& text) {
  for (const std::string& line : lines) {
    if (line.find(text) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST(ProbeSublevel, DiffLevelsSplitsAddsAndDels) {
  std::vector<std::uint64_t> added;
  std::vector<std::uint64_t> removed;
  DiffLevels({1, 3, 5}, {1, 2, 5, 9}, &added, &removed);
  EXPECT_EQ(added, (std::vector<std::uint64_t>{2, 9}));
  EXPECT_EQ(removed, (std::vector<std::uint64_t>{3}));
  DiffLevels({4}, {4}, &added, &removed);
  EXPECT_TRUE(added.empty());
  EXPECT_TRUE(removed.empty());
}

// The lobby and its two sublevels in, then out, twice: A sees each on the tick after B.
TEST(ProbeSublevel, CompleteStreamOnTimeMatchesEveryEvent) {
  SublevelMatcher matcher;
  std::vector<std::string> lines;
  std::uint64_t frame = 10;
  for (int pass = 0; pass < 2; ++pass) {
    for (bool enable : {true, false}) {
      for (std::uint64_t level : {kLobby, kLobbyArena, kLobbyCombat}) matcher.OnB(enable, level, frame);
      for (std::uint64_t level : {kLobbyCombat, kLobby, kLobbyArena}) matcher.OnA(enable, level, frame + 1, &lines);
      matcher.Expire(frame + 1, false, &lines);
      frame += 500;
    }
  }
  matcher.Expire(frame, true, &lines);
  EXPECT_EQ(matcher.b_events(), 12u);
  EXPECT_EQ(matcher.matched(), 12u);
  EXPECT_EQ(matcher.late(), 0u);
  EXPECT_EQ(matcher.missing(), 0u);
  EXPECT_EQ(matcher.extra(), 0u);
  EXPECT_TRUE(lines.empty());
  EXPECT_NE(matcher.Summary().find("b_events=12 a_matched=12 a_late=0 a_missing=0 a_extra=0 pending=0"),
            std::string::npos);
}

TEST(ProbeSublevel, ATwoTicksBehindIsLate) {
  SublevelMatcher matcher;
  std::vector<std::string> lines;
  matcher.OnB(true, kLobbyArena, 100);
  matcher.OnA(true, kLobbyArena, 102, &lines);
  EXPECT_EQ(matcher.matched(), 0u);
  EXPECT_EQ(matcher.late(), 1u);
  EXPECT_TRUE(AnyLineHas(lines, "probe_sub late kind=enable level=0x0000000000002222 lag=2"));
}

TEST(ProbeSublevel, AnEnableNeverSeenByAIsMissingAfterTheWindow) {
  SublevelMatcher matcher;
  std::vector<std::string> lines;
  matcher.OnB(false, kLobbyCombat, 100);
  matcher.Expire(100 + SublevelMatcher::kMissingAfterFrames, false, &lines);
  EXPECT_EQ(matcher.missing(), 0u);
  matcher.Expire(101 + SublevelMatcher::kMissingAfterFrames, false, &lines);
  EXPECT_EQ(matcher.missing(), 1u);
  EXPECT_TRUE(AnyLineHas(lines, "probe_sub missing kind=disable level=0x0000000000003333 frame=100"));
}

TEST(ProbeSublevel, AnAEventWithoutBIsExtraAndKindsDoNotCrossMatch) {
  SublevelMatcher matcher;
  std::vector<std::string> lines;
  matcher.OnB(true, kLobby, 5);
  matcher.OnA(false, kLobby, 6, &lines);  // a del cannot answer an enable
  matcher.OnA(true, kLobbyArena, 6, &lines);
  EXPECT_EQ(matcher.extra(), 2u);
  EXPECT_EQ(matcher.matched(), 0u);
  matcher.Expire(0, true, &lines);
  EXPECT_EQ(matcher.missing(), 1u);
}

TEST(ProbeSublevel, RepeatedEnablesOfOneLevelPairInOrder) {
  SublevelMatcher matcher;
  std::vector<std::string> lines;
  matcher.OnB(true, kLobby, 1);
  matcher.OnB(true, kLobby, 50);
  matcher.OnA(true, kLobby, 2, &lines);
  matcher.OnA(true, kLobby, 51, &lines);
  EXPECT_EQ(matcher.matched(), 2u);
  EXPECT_EQ(matcher.late(), 0u);
}
