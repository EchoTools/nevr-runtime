// The dedicated server's maps hold on the PC (server/map_hold.cpp and the Maps* half of server/join_hold.cpp): what a session create
// arms or refuses, how the channel's notes lift the hold, what a held accept does until then, and the group a legacy session start
// names. The real join_hold.cpp and map_hold.cpp run here, with a fake clock, a fake broadcaster send and a log sink; the game's
// hooks are never installed (Install is not called).
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "core/map_server_hold.h"
#include "runtime/hook/load_timing.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/server/join_hold.h"
#include "runtime/server/map_hold.h"

// The two game-facing symbols join_hold.cpp links that this test does not exercise.
const char* NevrCfgJoinHoldMs() { return nullptr; }

namespace nevr_load_timing {
std::uint64_t TimedLoad(std::uint64_t, std::uint64_t (*call)(void*, std::uint64_t, void*), void* gameSpace, std::uint64_t p2,
                        void* p3) {
  return call(gameSpace, p2, p3);
}
}  // namespace nevr_load_timing

namespace {

constexpr std::uint64_t kStartMs = 1000;
constexpr std::uint64_t kHalfCeilingMs = nevr_maps::kMapHoldCeilingMs / 2;
constexpr std::size_t kGuidBytes = 16;
constexpr std::size_t kGroupOffset = 0x10;
constexpr std::size_t kFrameBytes = kGroupOffset + kGuidBytes;
constexpr std::size_t kOneEntrant = 1;
constexpr std::size_t kTwice = 2;
constexpr std::uint64_t kSetBytes = 1000;  // a set size and the time it took, in any units: only their ratio matters
constexpr std::uint64_t kElapsedMs = 1000;

constexpr const char* kGroupText = "04030201-0605-0807-090a-0b0c0d0e0f10";
constexpr const char* kGroupUpper = "04030201-0605-0807-090A-0B0C0D0E0F10";
constexpr const char* kSession = "SESSION-1";
constexpr const char* kOtherGroup = "11111111-2222-3333-4444-555555555555";
constexpr std::uint8_t kAcceptTag = 0xAB;

std::uint64_t g_nowMs = kStartMs;
std::uint64_t FakeClock() { return g_nowMs; }

std::vector<std::pair<EchoVR::LogLevel, std::string>> g_lines;
void Sink(EchoVR::LogLevel level, const CHAR* message) { g_lines.emplace_back(level, message); }

std::vector<std::vector<std::uint8_t>> g_forwarded;
UINT64 FakeReceiveLocalEvent(EchoVR::Broadcaster*, EchoVR::SymbolId, const CHAR*, VOID* msg, UINT64 size) {
  const auto* bytes = static_cast<const std::uint8_t*>(msg);
  g_forwarded.emplace_back(bytes, bytes + size);
  return 0;
}

std::vector<std::string> g_requested;
bool g_requestAnswer = true;
bool FakeRequest(const std::string& group) {
  g_requested.push_back(group);
  return g_requestAnswer;
}

std::size_t Count(const std::string& needle, EchoVR::LogLevel level) {
  std::size_t n = 0;
  for (const auto& line : g_lines) {
    if (line.first == level && line.second.find(needle) != std::string::npos) ++n;
  }
  return n;
}
std::size_t CountAny(const std::string& needle) {
  std::size_t n = 0;
  for (const auto& line : g_lines) {
    if (line.second.find(needle) != std::string::npos) ++n;
  }
  return n;
}

class MapHoldTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_nowMs = kStartMs;
    g_lines.clear();
    g_forwarded.clear();
    g_requested.clear();
    g_requestAnswer = true;
    nevr_join_hold::SetClockForTest(&FakeClock);
    nevr_join_hold::ResetForTest();
    nevr_map_hold::SetRequestFetch(nullptr);
    SetBootLogSink(&Sink);
    previousSend_ = EchoVR::BroadcasterReceiveLocalEvent;
    EchoVR::BroadcasterReceiveLocalEvent = &FakeReceiveLocalEvent;
  }
  void TearDown() override {
    EchoVR::BroadcasterReceiveLocalEvent = previousSend_;
    SetBootLogSink(nullptr);
    nevr_map_hold::SetRequestFetch(nullptr);
    nevr_join_hold::ResetForTest();
    nevr_join_hold::SetClockForTest(nullptr);
  }

  // One accept arrives at the server: true when the hold queued it.
  static bool Accept() {
    const std::uint8_t bytes[] = {kAcceptTag};
    return nevr_join_hold::Admit(bytes, sizeof(bytes), kOneEntrant, kSession);
  }
  static void Release() { nevr_join_hold::ReleaseDue(&g_nowMs, kSession); }

  EchoVR::BroadcasterReceiveLocalEventFunc* previousSend_ = nullptr;
};

nevr_maps::MountReport Report(std::vector<std::pair<std::string, std::string>> notMounted = {},
                              std::vector<std::pair<std::string, std::string>> refused = {}) {
  nevr_maps::MountReport report;
  report.not_mounted = std::move(notMounted);
  report.refused = std::move(refused);
  return report;
}

// ---- GroupOfSessionStart ----

std::vector<std::uint8_t> Frame() {
  std::vector<std::uint8_t> frame(kFrameBytes, 0);
  for (std::size_t i = 0; i < kGuidBytes; ++i) frame[kGroupOffset + i] = static_cast<std::uint8_t>(i + 1);
  return frame;
}

TEST(GroupOfSessionStart, ReadsTheGroupGuidAtOffset0x10AsLowercaseText) {
  std::vector<std::uint8_t> frame = Frame();
  // The session GUID in the first 16 bytes is not the group: fill it with other bytes.
  for (std::size_t i = 0; i < kGuidBytes; ++i) frame[i] = static_cast<std::uint8_t>(kAcceptTag + i);
  EXPECT_EQ(nevr_map_hold::GroupOfSessionStart(frame.data(), frame.size()), kGroupText);
}

TEST(GroupOfSessionStart, ATrailingPayloadDoesNotChangeTheGroup) {
  std::vector<std::uint8_t> frame = Frame();
  frame.resize(frame.size() + kGuidBytes, kAcceptTag);
  EXPECT_EQ(nevr_map_hold::GroupOfSessionStart(frame.data(), frame.size()), kGroupText);
}

TEST(GroupOfSessionStart, ATooShortFrameOrNoFrameNamesNoGroup) {
  const std::vector<std::uint8_t> frame = Frame();
  EXPECT_EQ(nevr_map_hold::GroupOfSessionStart(nullptr, frame.size()), "");
  EXPECT_EQ(nevr_map_hold::GroupOfSessionStart(frame.data(), frame.size() - 1), "");
  EXPECT_EQ(nevr_map_hold::GroupOfSessionStart(frame.data(), 0), "");
}

TEST(GroupOfSessionStart, TheNilGroupNamesNoGroup) {
  std::vector<std::uint8_t> frame = Frame();
  for (std::size_t i = 0; i < kGuidBytes; ++i) frame[kGroupOffset + i] = 0;
  EXPECT_EQ(nevr_map_hold::GroupOfSessionStart(frame.data(), frame.size()), "");
}

// ---- OnSessionCreate ----

TEST_F(MapHoldTest, WithoutAChannelASessionCreateHoldsNothingAndSaysNothing) {
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  EXPECT_TRUE(g_lines.empty());
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_TRUE(g_requested.empty());
  EXPECT_FALSE(Accept());  // the gate was never enabled: the accept is forwarded now
}

TEST_F(MapHoldTest, ASessionWithNoGroupDropsTheHoldAndDoesNotAskForAFetch) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate("", kSession);
  EXPECT_TRUE(g_requested.empty());
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(Count("map hold off reason=no_group session=SESSION-1", EchoVR::LogLevel::Info), 1u);
  EXPECT_FALSE(Accept());
}

TEST_F(MapHoldTest, ANewSessionWithNoGroupLiftsTheHoldAnEarlierSessionArmed) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  ASSERT_TRUE(nevr_join_hold::MapsHoldingForTest());
  nevr_map_hold::OnSessionCreate("", kSession);
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
}

TEST_F(MapHoldTest, AGroupNotMountedIsArmedAndRequestedInLowercase) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupUpper, kSession);
  ASSERT_EQ(g_requested.size(), 1u);
  EXPECT_EQ(g_requested[0], kGroupText);
  EXPECT_TRUE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(Count(std::string("map hold armed group=") + kGroupText, EchoVR::LogLevel::Info), 1u);
  EXPECT_EQ(CountAny("map check group="), 0u);  // never mounted: a fetch, not a re-check
}

TEST_F(MapHoldTest, ARefusedRequestDropsTheHoldAtOnceWithAWarning) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  g_requestAnswer = false;
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  EXPECT_EQ(g_requested.size(), 1u);
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(Count(std::string("map hold off group=") + kGroupText + " reason=request_refused session=SESSION-1",
                  EchoVR::LogLevel::Warning),
            1u);
  EXPECT_FALSE(Accept());  // nothing holds the join
}

TEST_F(MapHoldTest, AMountedGroupIsCheckedAgainAndSaysSo) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  nevr_map_hold::NoteGroupMounted(kGroupText, Report());
  ASSERT_FALSE(nevr_join_hold::MapsHoldingForTest());
  g_lines.clear();
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  EXPECT_EQ(g_requested.size(), kOneEntrant + 1);  // fetched again: the check is the fetch of the manifest
  EXPECT_TRUE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(Count(std::string("map check group=") + kGroupText + " session=SESSION-1", EchoVR::LogLevel::Info), 1u);
}

TEST_F(MapHoldTest, ASecondCreateForTheSameStandingGroupKeepsItsDeadline) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  g_nowMs += kHalfCeilingMs;
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);  // the legacy frame names the group the protobuf one did
  EXPECT_EQ(Count("map hold armed", EchoVR::LogLevel::Info), 1u);
  g_nowMs += kHalfCeilingMs;  // the first arm's deadline, not a fresh one from the second create
  ASSERT_TRUE(Accept());
  Release();
  EXPECT_EQ(Count("map hold timeout", EchoVR::LogLevel::Warning), 1u);
}

TEST_F(MapHoldTest, ADifferentGroupReplacesTheHold) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  nevr_map_hold::OnSessionCreate(kOtherGroup, kSession);
  EXPECT_EQ(Count("map hold armed", EchoVR::LogLevel::Info), kTwice);
  // A mount of the first group no longer lifts it: the hold is the other group's.
  nevr_map_hold::NoteGroupMounted(kGroupText, Report());
  EXPECT_TRUE(nevr_join_hold::MapsHoldingForTest());
  nevr_map_hold::NoteGroupMounted(kOtherGroup, Report());
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
}

// ---- the held accept ----

TEST_F(MapHoldTest, AnAcceptWaitsForTheMountThenIsForwardedOnce) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  ASSERT_TRUE(Accept());
  Release();
  EXPECT_TRUE(g_forwarded.empty());
  g_nowMs += 1;
  nevr_map_hold::NoteGroupMounted(kGroupText, Report());
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(Count(std::string("map hold released group=") + kGroupText + " waited_ms=1 reason=mounted", EchoVR::LogLevel::Info), 1u);
  Release();
  ASSERT_EQ(g_forwarded.size(), 1u);
  EXPECT_EQ(g_forwarded[0], std::vector<std::uint8_t>{kAcceptTag});
  Release();
  EXPECT_EQ(g_forwarded.size(), 1u);
}

TEST_F(MapHoldTest, AnAcceptAfterTheMountPassesStraightThrough) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  nevr_map_hold::NoteGroupMounted(kGroupText, Report());
  EXPECT_FALSE(Accept());
}

TEST_F(MapHoldTest, TheHoldTimesOutAtTheCeilingWithNoPlanAndForwardsTheAcceptWithAWarning) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  ASSERT_TRUE(Accept());
  g_nowMs += nevr_maps::kMapHoldCeilingMs - 1;
  Release();
  EXPECT_TRUE(g_forwarded.empty());
  EXPECT_TRUE(nevr_join_hold::MapsHoldingForTest());
  g_nowMs += 1;
  Release();
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(Count(std::string("map hold timeout group=") + kGroupText, EchoVR::LogLevel::Warning), 1u);
  EXPECT_EQ(g_forwarded.size(), 1u);
}

TEST_F(MapHoldTest, ProgressShortensTheTimeoutToTheRemainingTransfer) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  nevr_maps::FetchProgress progress;
  progress.group = kGroupText;
  progress.set_bytes = kSetBytes;
  progress.bytes_to_fetch = kSetBytes;
  progress.bytes_fetched = kSetBytes;  // all of it, in the elapsed time: nothing remains, so the floor applies
  progress.elapsed_ms = kElapsedMs;
  nevr_map_hold::NoteFetchProgress(progress);
  ASSERT_TRUE(Accept());
  g_nowMs += nevr_maps::kMapHoldFloorMs - 1;
  Release();
  EXPECT_TRUE(nevr_join_hold::MapsHoldingForTest());
  g_nowMs += 1;
  Release();
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(Count("map hold timeout", EchoVR::LogLevel::Warning), 1u);
}

TEST_F(MapHoldTest, ProgressForAnotherGroupIsIgnored) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  nevr_maps::FetchProgress progress;
  progress.group = kOtherGroup;
  progress.set_bytes = kSetBytes;
  progress.bytes_to_fetch = kSetBytes;
  progress.bytes_fetched = kSetBytes;
  progress.elapsed_ms = kElapsedMs;
  nevr_map_hold::NoteFetchProgress(progress);
  ASSERT_TRUE(Accept());
  g_nowMs += nevr_maps::kMapHoldFloorMs;  // the held group's own plan is still unknown: the ceiling stands
  Release();
  EXPECT_TRUE(nevr_join_hold::MapsHoldingForTest());
}

TEST_F(MapHoldTest, ASessionEndDropsTheHoldAndItsHeldAccepts) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  ASSERT_TRUE(Accept());
  nevr_join_hold::OnSessionEnd("service_ended", kSession);
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(Count("join hold cleared dropped=1 cause=service_ended", EchoVR::LogLevel::Warning), 1u);
  Release();
  EXPECT_TRUE(g_forwarded.empty());
}

// ---- NoteGroupMounted ----

TEST_F(MapHoldTest, AMountOfANewerVersionHoldsTheJoinsUntilTheSessionEnds) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  ASSERT_TRUE(Accept());
  const nevr_maps::MountReport report =
      Report({{"arena_a", nevr_maps::kMountedOtherVersionToken}, {"arena_b", "skipped"}});
  nevr_map_hold::NoteGroupMounted(kGroupText, report);
  EXPECT_TRUE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(Count(std::string("map update group=") + kGroupText + " ids=arena_a reason=mounted_other_version",
                  EchoVR::LogLevel::Warning),
            1u);
  g_nowMs += nevr_maps::kMapHoldCeilingMs * kTwice;  // no deadline for an update
  Release();
  EXPECT_TRUE(g_forwarded.empty());
  EXPECT_TRUE(nevr_join_hold::MapsHoldingForTest());
  nevr_join_hold::OnSessionEnd("game_ended", kSession);
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
}

TEST_F(MapHoldTest, AnUpdatedGroupCountsAsMountedForTheNextSessionsCheck) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  nevr_map_hold::NoteGroupMounted(kGroupText, Report({{"arena_a", nevr_maps::kMountedOtherVersionToken}}));
  nevr_join_hold::OnSessionEnd("game_ended", kSession);
  g_lines.clear();
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  EXPECT_EQ(CountAny("map check group="), 1u);
}

TEST_F(MapHoldTest, SignedOnlyRefusalsWarnOncePerProcessWithTheCount) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  nevr_map_hold::NoteGroupMounted(kGroupText, Report({}, {{"arena_a", "signed_only"}, {"arena_b", "other"}, {"arena_c", "signed_only"}}));
  EXPECT_EQ(Count(std::string("signed_only group=") + kGroupText + " scripts_refused=2", EchoVR::LogLevel::Warning), 1u);
  nevr_map_hold::NoteGroupMounted(kGroupText, Report({}, {{"arena_a", "signed_only"}}));
  EXPECT_EQ(CountAny("signed_only group="), 1u);
}

// ---- NoteFetchFailed ----

TEST_F(MapHoldTest, AFailedCheckOfAMountedGroupLetsTheSessionGoAhead) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  nevr_map_hold::NoteGroupMounted(kGroupText, Report());
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);  // the re-check
  ASSERT_TRUE(Accept());
  nevr_map_hold::NoteFetchFailed(kGroupText);
  EXPECT_FALSE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(Count(std::string("map check failed group=") + kGroupText, EchoVR::LogLevel::Warning), 1u);
  Release();
  EXPECT_EQ(g_forwarded.size(), 1u);
}

TEST_F(MapHoldTest, AFailedFetchOfAGroupNeverMountedKeepsTheHoldToItsTimeout) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  nevr_map_hold::NoteFetchFailed(kGroupText);
  EXPECT_TRUE(nevr_join_hold::MapsHoldingForTest());
  EXPECT_EQ(CountAny("map check failed"), 0u);
}

TEST_F(MapHoldTest, AFailedCheckDoesNotLiftAHoldForAnUpdate) {
  nevr_map_hold::SetRequestFetch(&FakeRequest);
  nevr_map_hold::OnSessionCreate(kGroupText, kSession);
  nevr_map_hold::NoteGroupMounted(kGroupText, Report({{"arena_a", nevr_maps::kMountedOtherVersionToken}}));
  nevr_map_hold::NoteFetchFailed(kGroupText);
  EXPECT_TRUE(nevr_join_hold::MapsHoldingForTest());
}

}  // namespace
