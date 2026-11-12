// P-SKEW's client over recorded streams (src/runtime/maps/probe/probe_logic.cpp): what it logs and when it leaves.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "runtime/maps/probe/probe_logic.h"

using nevr_maps_probe::kLeaveRetryFrames;
using nevr_maps_probe::kServerProbeVersion;
using nevr_maps_probe::ParseProbeConfig;
using nevr_maps_probe::ProbePin;
using nevr_maps_probe::ProbeVersionOf;
using nevr_maps_probe::SkewAction;
using nevr_maps_probe::SkewClient;
using nevr_maps_probe::SkewInput;

namespace {

constexpr std::uint64_t kLobby = 0xABC0;
constexpr std::uint64_t kLevel = 0x5151;
constexpr std::int32_t kLobbyState = 5;
constexpr std::int32_t kLoadingLevel = 7;
constexpr std::int32_t kInGame = 9;

SkewInput At(std::int32_t state, std::uint64_t pin, std::uint64_t level = kLevel) {
  SkewInput in;
  in.lobby = kLobby;
  in.level = level;
  in.pin = pin;
  in.state = state;
  return in;
}

int CountLinesWith(const std::vector<std::string>& lines, const std::string& text) {
  int n = 0;
  for (const std::string& line : lines) n += line.find(text) != std::string::npos ? 1 : 0;
  return n;
}

}  // namespace

TEST(ProbeSkew, PinsRoundTripAndUnknownPinsAreUnknown) {
  EXPECT_EQ(ProbeVersionOf(ProbePin(kServerProbeVersion)), 2);
  EXPECT_EQ(ProbeVersionOf(ProbePin(1)), 1);
  EXPECT_NE(ProbePin(1), ProbePin(2));
  EXPECT_EQ(ProbeVersionOf(0), -1);
  EXPECT_EQ(ProbeVersionOf(0x1234), -1);
}

// skew_expect: none logs the pin, the loading transition and the load, and never leaves.
TEST(ProbeSkew, ExpectNoneLogsTheOrderAndStays) {
  SkewClient client(ParseProbeConfig("skew: on\nskew_expect: none"));
  std::vector<std::string> lines;
  const std::uint64_t pin = ProbePin(2);
  EXPECT_FALSE(client.Tick(At(kLobbyState, pin), 1, &lines).leave);
  EXPECT_FALSE(client.Tick(At(kLoadingLevel, pin), 2, &lines).leave);
  client.OnEnable(kLevel, 3, &lines);
  EXPECT_FALSE(client.Tick(At(kInGame, pin), 4, &lines).leave);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_NE(lines[0].find("probe_skew seen level=0x0000000000005151 pin="), std::string::npos);
  EXPECT_NE(lines[0].find("frame=1 state=5"), std::string::npos);
  EXPECT_EQ(lines[1], "probe_skew loading level=0x0000000000005151 frame=2");
  EXPECT_EQ(lines[2], "probe_skew loaded level=0x0000000000005151 frame=3");
}

TEST(ProbeSkew, WrongVersionLeavesBeforeLoadWithTheVersionNotice) {
  SkewClient client(ParseProbeConfig("skew_expect: probe@1"));
  std::vector<std::string> lines;
  const SkewAction action = client.Tick(At(kLoadingLevel, ProbePin(2)), 10, &lines);
  ASSERT_TRUE(action.leave);
  EXPECT_EQ(action.notice, "This match uses probe version 2 (yours: 1)");
  EXPECT_LE(action.notice.size(), 63u);
  client.OnLeaveResult(true, 10, &lines);
  EXPECT_EQ(CountLinesWith(lines, "probe_skew leave reason=map_skew before_load=1 frame=10"), 1);
  EXPECT_FALSE(client.Tick(At(kLoadingLevel, ProbePin(2)), 11, &lines).leave);
}

TEST(ProbeSkew, MissingLeavesWithTheNotInstalledNotice) {
  SkewClient client(ParseProbeConfig("skew_expect: missing"));
  std::vector<std::string> lines;
  const SkewAction action = client.Tick(At(kLoadingLevel, ProbePin(2)), 1, &lines);
  ASSERT_TRUE(action.leave);
  EXPECT_NE(action.notice.find("not installed"), std::string::npos);
}

TEST(ProbeSkew, MatchingVersionStays) {
  SkewClient client(ParseProbeConfig("skew_expect: probe@2"));
  std::vector<std::string> lines;
  EXPECT_FALSE(client.Tick(At(kLoadingLevel, ProbePin(2)), 1, &lines).leave);
  EXPECT_EQ(CountLinesWith(lines, "probe_skew match version=2"), 1);
}

TEST(ProbeSkew, LeaveAfterTheLoadSaysSo) {
  SkewClient client(ParseProbeConfig("skew_expect: probe@1"));
  std::vector<std::string> lines;
  EXPECT_FALSE(client.Tick(At(kInGame, 0), 1, &lines).leave);  // the pin is not there yet
  EXPECT_EQ(CountLinesWith(lines, "probe_skew unpinned"), 1);
  client.OnEnable(kLevel, 2, &lines);
  const SkewAction action = client.Tick(At(kInGame, ProbePin(2)), 3, &lines);
  ASSERT_TRUE(action.leave);
  client.OnLeaveResult(true, 3, &lines);
  EXPECT_EQ(CountLinesWith(lines, "before_load=0"), 1);
}

TEST(ProbeSkew, AFailedScheduleRetriesWithoutRewritingTheNoticeThenGivesUp) {
  SkewClient client(ParseProbeConfig("skew_expect: probe@1"));
  std::vector<std::string> lines;
  SkewAction action = client.Tick(At(kLoadingLevel, ProbePin(2)), 1, &lines);
  ASSERT_TRUE(action.leave);
  EXPECT_FALSE(action.notice.empty());
  client.OnLeaveResult(false, 1, &lines);
  action = client.Tick(At(kLoadingLevel, ProbePin(2)), 2, &lines);
  ASSERT_TRUE(action.leave);
  EXPECT_TRUE(action.notice.empty());
  client.OnLeaveResult(false, 1 + kLeaveRetryFrames, &lines);
  EXPECT_EQ(CountLinesWith(lines, "probe_skew leave reason=map_skew failed=1"), 1);
  EXPECT_FALSE(client.Tick(At(kLoadingLevel, ProbePin(2)), 3 + kLeaveRetryFrames, &lines).leave);
}

TEST(ProbeSkew, LeavingTheSessionDecidesTheNextOneAgain) {
  SkewClient client(ParseProbeConfig("skew_expect: probe@1"));
  std::vector<std::string> lines;
  ASSERT_TRUE(client.Tick(At(kLoadingLevel, ProbePin(2)), 1, &lines).leave);
  client.OnLeaveResult(true, 1, &lines);
  EXPECT_FALSE(client.Tick(At(kLobbyState, 0, 0), 2, &lines).leave);
  EXPECT_TRUE(client.Tick(At(kLoadingLevel, ProbePin(2)), 3, &lines).leave);
}

TEST(ProbeSkew, AnEnableOfAnotherLevelIsNotTheLoad) {
  SkewClient client(ParseProbeConfig("skew_expect: none"));
  std::vector<std::string> lines;
  client.Tick(At(kLoadingLevel, ProbePin(2)), 1, &lines);
  client.OnEnable(0x9999, 2, &lines);
  EXPECT_EQ(CountLinesWith(lines, "probe_skew loaded"), 0);
}
