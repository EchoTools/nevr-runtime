// The custom-map probes' game-free half (runtime/probe/probe_logic.h): the maps_probe.yaml reader, the level diff, the P-SUBLEVEL
// matcher, the P-SKEW client and the P-RULES store check. Nothing here touches the game.

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "abi/echovr.h"
#include "core/map_session_pin.h"
#include "runtime/probe/probe_logic.h"

namespace {

namespace probe = nevr_map_probe;

using Lines = std::vector<std::string>;

constexpr std::uint64_t kLobby = 0xAA55;
constexpr std::uint64_t kLevelA = 0x1111;
constexpr std::uint64_t kLevelB = 0x2222;
constexpr std::uint64_t kFrame0 = 100;
constexpr std::uint64_t kLateLag = 5;  // frames an A comes after its B, past the on-time window
constexpr std::uint32_t kTwoDigitVersion = 42;
constexpr std::int32_t kInGame = static_cast<std::int32_t>(EchoVR::NetGameState::InGame);
constexpr std::int32_t kLobbyState = static_cast<std::int32_t>(EchoVR::NetGameState::Lobby);
constexpr std::int32_t kLoadingLevel = static_cast<std::int32_t>(EchoVR::NetGameState::LoadingLevel);

bool Has(const Lines& lines, const std::string& needle) {
  return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; });
}

probe::SkewInput Session(std::uint64_t pin, std::int32_t state = kInGame) {
  probe::SkewInput in;
  in.lobby = kLobby;
  in.level = kLevelA;
  in.pin = pin;
  in.state = state;
  return in;
}

probe::ProbeConfig Expect(probe::SkewExpect expect, std::uint32_t version = 0) {
  probe::ProbeConfig config;
  config.skew = true;
  config.skew_expect = expect;
  config.skew_version = version;
  return config;
}

// ---- the config reader ----

TEST(MapProbeConfig, EmptyAndAbsentTextTurnEveryProbeOff) {
  const probe::ProbeConfig config = probe::ParseProbeConfig("");
  EXPECT_FALSE(config.rules);
  EXPECT_FALSE(config.skew);
  EXPECT_FALSE(config.sublevel);
  EXPECT_EQ(config.skew_expect, probe::SkewExpect::kNone);
  EXPECT_EQ(config.ignored_lines, 0u);
}

TEST(MapProbeConfig, EachSwitchWordIsRead) {
  const probe::ProbeConfig config = probe::ParseProbeConfig("rules: on\nskew: yes\nsublevel: true\n");
  EXPECT_TRUE(config.rules);
  EXPECT_TRUE(config.skew);
  EXPECT_TRUE(config.sublevel);
  const probe::ProbeConfig off = probe::ParseProbeConfig("rules: off\nskew: no\nsublevel: 0\n");
  EXPECT_FALSE(off.rules);
  EXPECT_FALSE(off.skew);
  EXPECT_FALSE(off.sublevel);
  EXPECT_TRUE(probe::ParseProbeConfig("rules: 1\n").rules);
}

TEST(MapProbeConfig, CommentsQuotesCaseAndCrlfAreHandled) {
  const probe::ProbeConfig config =
      probe::ParseProbeConfig("# a comment\r\nRULES: \"On\"   # trailing\r\n  Skew :  'true'\r\n\r\n");
  EXPECT_TRUE(config.rules);
  EXPECT_TRUE(config.skew);
  EXPECT_EQ(config.ignored_lines, 0u);
}

TEST(MapProbeConfig, ABomIsSkipped) {
  EXPECT_TRUE(probe::ParseProbeConfig("\xEF\xBB\xBF" "sublevel: on\n").sublevel);
}

TEST(MapProbeConfig, UnknownKeysBadValuesAndLinesWithoutAColonAreCountedAndChangeNothing) {
  const probe::ProbeConfig config = probe::ParseProbeConfig("bogus: on\nrules: maybe\nno colon here\nskew_expect: nope\n");
  EXPECT_FALSE(config.rules);
  EXPECT_EQ(config.ignored_lines, 4u);
  EXPECT_EQ(config.skew_expect, probe::SkewExpect::kNone);
}

TEST(MapProbeConfig, SkewExpectReadsNoneMissingAndAVersion) {
  EXPECT_EQ(probe::ParseProbeConfig("skew_expect: missing\n").skew_expect, probe::SkewExpect::kMissing);
  const probe::ProbeConfig v1 = probe::ParseProbeConfig("skew_expect: probe@1\n");
  EXPECT_EQ(v1.skew_expect, probe::SkewExpect::kVersion);
  EXPECT_EQ(v1.skew_version, 1u);
  EXPECT_EQ(probe::ParseProbeConfig("skew_expect: probe@42\n").skew_version, kTwoDigitVersion);
  EXPECT_EQ(probe::ParseProbeConfig("skew_expect: none\n").skew_expect, probe::SkewExpect::kNone);
}

TEST(MapProbeConfig, SkewExpectRefusesMalformedVersions) {
  for (const char* bad : {"skew_expect: probe@\n", "skew_expect: probe@123\n", "skew_expect: probe@x\n",
                          "skew_expect: probe@-1\n", "skew_expect: probe1\n"}) {
    const probe::ProbeConfig config = probe::ParseProbeConfig(bad);
    EXPECT_EQ(config.skew_expect, probe::SkewExpect::kNone) << bad;
    EXPECT_EQ(config.ignored_lines, 1u) << bad;
  }
}

TEST(MapProbeConfig, SkewExpectNamesAreStable) {
  EXPECT_STREQ(probe::SkewExpectName(probe::SkewExpect::kNone), "none");
  EXPECT_STREQ(probe::SkewExpectName(probe::SkewExpect::kMissing), "missing");
  EXPECT_STREQ(probe::SkewExpectName(probe::SkewExpect::kVersion), "version");
}

// ---- the level diff ----

TEST(MapProbeDiff, ReportsAddedAndRemovedLevels) {
  std::vector<std::uint64_t> added;
  std::vector<std::uint64_t> removed;
  probe::DiffLevels({1, 2, 3}, {2, 3, 4, 5}, &added, &removed);
  EXPECT_EQ(added, (std::vector<std::uint64_t>{4, 5}));
  EXPECT_EQ(removed, (std::vector<std::uint64_t>{1}));
}

TEST(MapProbeDiff, ClearsTheOutputsAndIsEmptyForEqualTables) {
  std::vector<std::uint64_t> added = {9};
  std::vector<std::uint64_t> removed = {9};
  probe::DiffLevels({7, 8}, {7, 8}, &added, &removed);
  EXPECT_TRUE(added.empty());
  EXPECT_TRUE(removed.empty());
}

TEST(MapProbeDiff, HexLevelIsSixteenDigits) {
  EXPECT_EQ(probe::HexLevel(0xABC), "0x0000000000000abc");
}

// ---- P-SUBLEVEL ----

TEST(MapProbeSublevel, AnAOnTheNextTickMatchesItsB) {
  probe::SublevelMatcher matcher;
  Lines lines;
  matcher.OnB(true, kLevelA, kFrame0);
  matcher.OnA(true, kLevelA, kFrame0 + 1, &lines);
  EXPECT_EQ(matcher.matched(), 1u);
  EXPECT_EQ(matcher.late(), 0u);
  EXPECT_TRUE(lines.empty());
}

TEST(MapProbeSublevel, AnASameTickAsItsBAlsoMatches) {
  probe::SublevelMatcher matcher;
  Lines lines;
  matcher.OnB(false, kLevelA, kFrame0);
  matcher.OnA(false, kLevelA, kFrame0, &lines);
  EXPECT_EQ(matcher.matched(), 1u);
}

TEST(MapProbeSublevel, ALateAIsCountedWithItsLag) {
  probe::SublevelMatcher matcher;
  Lines lines;
  matcher.OnB(true, kLevelA, kFrame0);
  matcher.OnA(true, kLevelA, kFrame0 + kLateLag, &lines);
  EXPECT_EQ(matcher.matched(), 0u);
  EXPECT_EQ(matcher.late(), 1u);
  EXPECT_TRUE(Has(lines, "probe_sub late kind=enable level=0x0000000000001111 lag=" + std::to_string(kLateLag)));
}

TEST(MapProbeSublevel, AnAWithNoBIsExtraAndNamesItsKind) {
  probe::SublevelMatcher matcher;
  Lines lines;
  matcher.OnA(true, kLevelA, kFrame0, &lines);
  matcher.OnA(false, kLevelB, kFrame0, &lines);
  EXPECT_EQ(matcher.extra(), 2u);
  EXPECT_TRUE(Has(lines, "probe_sub extra kind=add level=0x0000000000001111"));
  EXPECT_TRUE(Has(lines, "probe_sub extra kind=del level=0x0000000000002222"));
}

TEST(MapProbeSublevel, AnEnableDoesNotPairWithADelOfTheSameLevel) {
  probe::SublevelMatcher matcher;
  Lines lines;
  matcher.OnB(true, kLevelA, kFrame0);
  matcher.OnA(false, kLevelA, kFrame0 + 1, &lines);
  EXPECT_EQ(matcher.extra(), 1u);
  EXPECT_EQ(matcher.matched(), 0u);
}

TEST(MapProbeSublevel, ABWithNoAIsMissingOnlyAfterTheWindow) {
  probe::SublevelMatcher matcher;
  Lines lines;
  matcher.OnB(true, kLevelA, kFrame0);
  matcher.Expire(kFrame0 + probe::SublevelMatcher::kMissingAfterFrames, false, &lines);
  EXPECT_EQ(matcher.missing(), 0u);
  matcher.Expire(kFrame0 + probe::SublevelMatcher::kMissingAfterFrames + 1, false, &lines);
  EXPECT_EQ(matcher.missing(), 1u);
  EXPECT_TRUE(Has(lines, "probe_sub missing kind=enable level=0x0000000000001111 frame=100"));
  matcher.Expire(kFrame0 * 100, false, &lines);
  EXPECT_EQ(matcher.missing(), 1u);  // an expired B is gone, not counted twice
}

TEST(MapProbeSublevel, ExpireAllEndsEveryPendingB) {
  probe::SublevelMatcher matcher;
  Lines lines;
  matcher.OnB(true, kLevelA, kFrame0);
  matcher.OnB(false, kLevelB, kFrame0 + 1);
  matcher.Expire(0, true, &lines);
  EXPECT_EQ(matcher.missing(), 2u);
  EXPECT_TRUE(Has(lines, "kind=disable level=0x0000000000002222"));
}

TEST(MapProbeSublevel, ASecondPendingBOfTheSameLevelPairsOnceEach) {
  probe::SublevelMatcher matcher;
  Lines lines;
  matcher.OnB(true, kLevelA, kFrame0);
  matcher.OnB(true, kLevelA, kFrame0 + 2);
  matcher.OnA(true, kLevelA, kFrame0 + 1, &lines);
  matcher.OnA(true, kLevelA, kFrame0 + 3, &lines);
  EXPECT_EQ(matcher.matched(), 2u);
  matcher.OnA(true, kLevelA, kFrame0 + 4, &lines);
  EXPECT_EQ(matcher.extra(), 1u);
}

TEST(MapProbeSublevel, SummaryCarriesEveryCounter) {
  probe::SublevelMatcher matcher;
  Lines lines;
  matcher.OnB(true, kLevelA, kFrame0);
  matcher.OnB(true, kLevelB, kFrame0);
  matcher.OnA(true, kLevelA, kFrame0 + 1, &lines);
  matcher.OnA(true, 0x3333, kFrame0 + 1, &lines);
  EXPECT_EQ(matcher.Summary(), "probe_sub summary b_events=2 a_matched=1 a_late=0 a_missing=0 a_extra=1 pending=1");
  EXPECT_EQ(matcher.b_events(), 2u);
}

// ---- P-SKEW: the pins ----

TEST(MapProbePin, ProbePinsAreDistinctAndReadBack) {
  EXPECT_NE(probe::ProbePin(1), probe::ProbePin(2));
  EXPECT_EQ(probe::ProbeVersionOf(probe::ProbePin(0)), 0);
  EXPECT_EQ(probe::ProbeVersionOf(probe::ProbePin(probe::kServerProbeVersion)), static_cast<int>(probe::kServerProbeVersion));
  EXPECT_EQ(probe::ProbeVersionOf(probe::ProbePin(probe::kMaxProbeVersion)), static_cast<int>(probe::kMaxProbeVersion));
}

TEST(MapProbePin, AnUnknownOrAbsentPinHasNoVersion) {
  EXPECT_EQ(probe::ProbeVersionOf(0), -1);
  EXPECT_EQ(probe::ProbeVersionOf(0xDEADBEEF), -1);
  EXPECT_EQ(probe::ProbeVersionOf(probe::ProbePin(probe::kMaxProbeVersion + 1)), -1);
}

// ---- P-SKEW: the client ----

TEST(MapProbeSkew, ExpectNoneLogsWhatItSeesAndNeverLeaves) {
  probe::SkewClient client(Expect(probe::SkewExpect::kNone));
  Lines lines;
  const probe::SkewAction action = client.Tick(Session(probe::ProbePin(2)), kFrame0, &lines);
  EXPECT_FALSE(action.leave);
  EXPECT_TRUE(Has(lines, "probe_skew seen level=0x0000000000001111"));
  EXPECT_FALSE(Has(lines, "probe_skew match"));
}

TEST(MapProbeSkew, AMatchingVersionStaysAndSaysSo) {
  probe::SkewClient client(Expect(probe::SkewExpect::kVersion, 2));
  Lines lines;
  const probe::SkewAction action = client.Tick(Session(probe::ProbePin(2)), kFrame0, &lines);
  EXPECT_FALSE(action.leave);
  EXPECT_TRUE(Has(lines, "probe_skew match version=2"));
}

TEST(MapProbeSkew, AnotherVersionLeavesWithAShortNotice) {
  probe::SkewClient client(Expect(probe::SkewExpect::kVersion, 1));
  Lines lines;
  const probe::SkewAction action = client.Tick(Session(probe::ProbePin(2)), kFrame0, &lines);
  EXPECT_TRUE(action.leave);
  EXPECT_EQ(action.notice, "This match uses probe version 2 (yours: 1)");
  EXPECT_LE(action.notice.size(), nevr_map_session_pin::kNoticeLineMax);
}

TEST(MapProbeSkew, APinThatIsNoProbeVersionLeavesAsUnknown) {
  probe::SkewClient client(Expect(probe::SkewExpect::kVersion, 1));
  Lines lines;
  const probe::SkewAction action = client.Tick(Session(0xDEADBEEF), kFrame0, &lines);
  EXPECT_TRUE(action.leave);
  EXPECT_EQ(action.notice, "This match uses an unknown probe version (yours: 1)");
  EXPECT_LE(action.notice.size(), nevr_map_session_pin::kNoticeLineMax);
}

TEST(MapProbeSkew, ExpectMissingLeavesOnAnyPin) {
  probe::SkewClient client(Expect(probe::SkewExpect::kMissing));
  Lines lines;
  const probe::SkewAction known = client.Tick(Session(probe::ProbePin(2)), kFrame0, &lines);
  EXPECT_TRUE(known.leave);
  EXPECT_EQ(known.notice, "This match uses probe version 2: not installed");
  probe::SkewClient other(Expect(probe::SkewExpect::kMissing));
  const probe::SkewAction unknown = other.Tick(Session(0xDEADBEEF), kFrame0, &lines);
  EXPECT_TRUE(unknown.leave);
  EXPECT_EQ(unknown.notice, "This match uses a probe version that is not installed");
  EXPECT_LE(unknown.notice.size(), nevr_map_session_pin::kNoticeLineMax);
}

TEST(MapProbeSkew, AnUnpinnedSessionIsLoggedAndStays) {
  probe::SkewClient client(Expect(probe::SkewExpect::kVersion, 1));
  Lines lines;
  const probe::SkewAction action = client.Tick(Session(0), kFrame0, &lines);
  EXPECT_FALSE(action.leave);
  EXPECT_TRUE(Has(lines, "probe_skew unpinned level=0x0000000000001111"));
}

TEST(MapProbeSkew, TheNoticeIsOfferedOnceAndTheLeaveRepeatsUntilItIsScheduled) {
  probe::SkewClient client(Expect(probe::SkewExpect::kMissing));
  Lines lines;
  const probe::SkewAction first = client.Tick(Session(probe::ProbePin(2)), kFrame0, &lines);
  ASSERT_TRUE(first.leave);
  EXPECT_FALSE(first.notice.empty());
  client.OnLeaveResult(false, kFrame0, &lines);
  const probe::SkewAction second = client.Tick(Session(probe::ProbePin(2)), kFrame0 + 1, &lines);
  EXPECT_TRUE(second.leave);
  EXPECT_TRUE(second.notice.empty());
  client.OnLeaveResult(true, kFrame0 + 1, &lines);
  EXPECT_TRUE(Has(lines, "probe_skew leave reason=map_skew before_load=1 frame=101"));
  const probe::SkewAction third = client.Tick(Session(probe::ProbePin(2)), kFrame0 + 2, &lines);
  EXPECT_FALSE(third.leave);
}

TEST(MapProbeSkew, ALeaveThatNeverSchedulesGivesUpAtTheDeadline) {
  probe::SkewClient client(Expect(probe::SkewExpect::kMissing));
  Lines lines;
  client.Tick(Session(probe::ProbePin(2)), kFrame0, &lines);
  client.OnLeaveResult(false, kFrame0 + probe::kLeaveRetryFrames - 1, &lines);
  EXPECT_FALSE(Has(lines, "failed=1"));
  EXPECT_TRUE(client.Tick(Session(probe::ProbePin(2)), kFrame0 + 1, &lines).leave);
  client.OnLeaveResult(false, kFrame0 + probe::kLeaveRetryFrames, &lines);
  EXPECT_TRUE(Has(lines, "probe_skew leave reason=map_skew failed=1 before_load=1"));
  EXPECT_FALSE(client.Tick(Session(probe::ProbePin(2)), kFrame0 + probe::kLeaveRetryFrames + 1, &lines).leave);
}

TEST(MapProbeSkew, TheLeaveSaysWhetherTheLevelHadLoaded) {
  probe::SkewClient client(Expect(probe::SkewExpect::kMissing));
  Lines lines;
  client.Tick(Session(probe::ProbePin(2), kLoadingLevel), kFrame0, &lines);
  client.OnEnable(kLevelA, kFrame0 + 1, &lines);
  EXPECT_TRUE(Has(lines, "probe_skew loaded level=0x0000000000001111 frame=101"));
  client.Tick(Session(probe::ProbePin(2)), kFrame0 + 1, &lines);
  client.OnLeaveResult(true, kFrame0 + 1, &lines);
  EXPECT_TRUE(Has(lines, "before_load=0"));
}

TEST(MapProbeSkew, OnlyTheSessionsLevelCountsAsLoadedAndOnlyOnce) {
  probe::SkewClient client(Expect(probe::SkewExpect::kNone));
  Lines lines;
  client.Tick(Session(0), kFrame0, &lines);
  client.OnEnable(kLevelB, kFrame0, &lines);
  client.OnEnable(0, kFrame0, &lines);
  EXPECT_FALSE(Has(lines, "probe_skew loaded"));
  client.OnEnable(kLevelA, kFrame0, &lines);
  client.OnEnable(kLevelA, kFrame0 + 1, &lines);
  EXPECT_EQ(std::count_if(lines.begin(), lines.end(), [](const std::string& l) { return l.find("probe_skew loaded") != std::string::npos; }), 1);
}

TEST(MapProbeSkew, LeavingTheSessionForgetsTheDecision) {
  probe::SkewClient client(Expect(probe::SkewExpect::kMissing));
  Lines lines;
  ASSERT_TRUE(client.Tick(Session(probe::ProbePin(2)), kFrame0, &lines).leave);
  probe::SkewInput lobby = Session(probe::ProbePin(2), kLobbyState);
  EXPECT_FALSE(client.Tick(lobby, kFrame0 + 1, &lines).leave);
  client.OnLeaveResult(true, kFrame0 + 1, &lines);  // no leave is pending any more: no line
  EXPECT_FALSE(Has(lines, "probe_skew leave"));
  EXPECT_TRUE(client.Tick(Session(probe::ProbePin(2)), kFrame0 + 2, &lines).leave);  // a new session decides again
}

TEST(MapProbeSkew, ALoadingLevelTransitionIsLoggedOnce) {
  probe::SkewClient client(Expect(probe::SkewExpect::kNone));
  Lines lines;
  client.Tick(Session(0, kLoadingLevel), kFrame0, &lines);
  client.Tick(Session(0, kLoadingLevel), kFrame0 + 1, &lines);
  EXPECT_EQ(std::count_if(lines.begin(), lines.end(), [](const std::string& l) { return l.find("probe_skew loading") != std::string::npos; }), 1);
}

TEST(MapProbeSkew, NothingIsLoggedWithoutALobby) {
  probe::SkewClient client(Expect(probe::SkewExpect::kMissing));
  Lines lines;
  probe::SkewInput none;
  none.state = kInGame;
  EXPECT_FALSE(client.Tick(none, kFrame0, &lines).leave);
  EXPECT_TRUE(lines.empty());
}

// ---- P-RULES: the store check ----

constexpr probe::ZoneStore kStore = {0x100, {0x48, 0x89, 0x35}, 0x1000};

// A 7-byte store at kStore.site_rva whose rel32 lands on `global_rva`.
std::vector<std::uint8_t> StoreBytes(std::uintptr_t global_rva, std::uint8_t opcode2 = 0x35) {
  std::vector<std::uint8_t> bytes = {0x48, 0x89, opcode2, 0, 0, 0, 0};
  const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::int64_t>(global_rva) -
                                                      static_cast<std::int64_t>(kStore.site_rva + probe::kStoreSize));
  std::memcpy(bytes.data() + probe::kStoreOpcodeBytes, &rel, sizeof(rel));
  return bytes;
}

TEST(MapProbeRules, AStoreThatEncodesItsGlobalIsAccepted) {
  EXPECT_TRUE(probe::StoreEncodes(StoreBytes(kStore.global_rva).data(), kStore));
}

TEST(MapProbeRules, AStoreOfAnotherGlobalIsRefused) {
  EXPECT_FALSE(probe::StoreEncodes(StoreBytes(kStore.global_rva + 8).data(), kStore));
}

TEST(MapProbeRules, AStoreWithAnotherOpcodeIsRefused) {
  EXPECT_FALSE(probe::StoreEncodes(StoreBytes(kStore.global_rva, 0x3D).data(), kStore));
}

TEST(MapProbeRules, ExpectedStoreBytesAreWhatStoreEncodesAccepts) {
  std::uint8_t expected[probe::kStoreSize];
  probe::ExpectedStoreBytes(kStore, expected);
  EXPECT_TRUE(probe::StoreEncodes(expected, kStore));
  EXPECT_EQ(std::vector<std::uint8_t>(expected, expected + probe::kStoreSize), StoreBytes(kStore.global_rva));
}

TEST(MapProbeRules, ABackwardsDisplacementIsSignExtended) {
  const probe::ZoneStore distant = {0x5000, {0x48, 0x89, 0x35}, 0x40};
  std::vector<std::uint8_t> bytes = {0x48, 0x89, 0x35, 0, 0, 0, 0};
  const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::int64_t>(distant.global_rva) -
                                                      static_cast<std::int64_t>(distant.site_rva + probe::kStoreSize));
  ASSERT_LT(rel, 0);
  std::memcpy(bytes.data() + probe::kStoreOpcodeBytes, &rel, sizeof(rel));
  EXPECT_TRUE(probe::StoreEncodes(bytes.data(), distant));
}

// ---- Rule 6: the fields of a pin refusal ----

constexpr std::uint64_t kPinVa = 0x140551070;
constexpr std::uint8_t kPinExpected[] = {0x40, 0x55, 0x53, 0x56};
constexpr std::uint8_t kPinActual[] = {0xC3, 0x90, 0x90, 0x90};

TEST(MapProbePinRefusal, NamesTheVaTheExpectedAndTheActualBytes) {
  EXPECT_EQ(probe::NotPinnedFields(kPinVa, kPinExpected, kPinActual, sizeof(kPinExpected)),
            "va=0x140551070 expected=40555356 actual=c3909090");
}

TEST(MapProbePinRefusal, AnUnreadableTargetSaysSo) {
  EXPECT_EQ(probe::NotPinnedFields(kPinVa, kPinExpected, nullptr, sizeof(kPinExpected)),
            "va=0x140551070 expected=40555356 actual=unreadable");
}

}  // namespace
