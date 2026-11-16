// PLAN-v5 R6-a-2b (docs/adr/0045 decision 10; N152): the server's session-start hold. The pure rules only: the hold's answers and its
// deadline, the level and game type read from a session start frame. GTest, like test_join_hold.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "cdn/cdn_demand.h"
#include "runtime/server/game_type_table.h"
#include "runtime/server/session_demand_rules.h"

namespace {

using nevr_session_demand::Hold;
using nevr_session_demand::Step;
using nevr_session_demand::kDemandGraceMs;
using nevr_session_demand::kSessionDemandHoldMs;

constexpr std::uint64_t kNowMs = 0;
constexpr std::uint64_t kJustBeforeDeadlineMs = kSessionDemandHoldMs - 1;
constexpr std::uint64_t kNamedHoldSumMs = 130000;  // kServerDemandMs (120000) + kDemandGraceMs (10000), PLAN-v5 1.4
constexpr std::uint64_t kLevelDecimal = 5764607523034234880ULL;
constexpr std::uint64_t kNegativeOneBits = 0xFFFFFFFFFFFFFFFFULL;
constexpr std::uint64_t kMplArenaAHash = 0x576ed3f8428ebc4bULL;

std::vector<std::uint8_t> FrameWith(const std::string& json, bool nulTerminated) {
  std::vector<std::uint8_t> frame(nevr_game_type_table::kSessionStartHeaderSize, 0);
  frame.insert(frame.end(), json.begin(), json.end());
  if (nulTerminated) frame.push_back(0);
  return frame;
}

}  // namespace

// B1
TEST(SessionDemandHold, BeginHolds) {
  Hold hold(kSessionDemandHoldMs);
  const std::uint64_t ticket = hold.Begin(kNowMs);
  EXPECT_TRUE(hold.Holding());
  EXPECT_TRUE(hold.Admit());
  EXPECT_EQ(hold.Ticket(), ticket);
}

// B2
TEST(SessionDemandHold, AnOkAnswerForTheCurrentTicketForwards) {
  Hold hold(kSessionDemandHoldMs);
  const std::uint64_t ticket = hold.Begin(kNowMs);
  EXPECT_EQ(hold.Finish(ticket, true), Step::kForward);
  EXPECT_FALSE(hold.Holding());
  EXPECT_FALSE(hold.Admit());
}

// B3
TEST(SessionDemandHold, AFailedAnswerFails) {
  Hold hold(kSessionDemandHoldMs);
  const std::uint64_t ticket = hold.Begin(kNowMs);
  EXPECT_EQ(hold.Finish(ticket, false), Step::kFail);
  EXPECT_FALSE(hold.Holding());
}

// B4
TEST(SessionDemandHold, AStaleTicketIsIgnored) {
  Hold hold(kSessionDemandHoldMs);
  const std::uint64_t first = hold.Begin(kNowMs);
  const std::uint64_t second = hold.Begin(kNowMs);
  EXPECT_EQ(first, 1u);
  EXPECT_EQ(second, first + 1);
  EXPECT_EQ(hold.Finish(first, true), Step::kNone);
  EXPECT_TRUE(hold.Holding());
  EXPECT_EQ(hold.Finish(second, true), Step::kForward);
}

// B5
TEST(SessionDemandHold, TheDeadlineFailsAndNeverForwards) {
  Hold hold(kSessionDemandHoldMs);
  const std::uint64_t ticket = hold.Begin(kNowMs);
  EXPECT_EQ(hold.Poll(kJustBeforeDeadlineMs), Step::kNone);
  EXPECT_EQ(hold.Poll(kSessionDemandHoldMs), Step::kFail);
  EXPECT_EQ(hold.Finish(ticket, true), Step::kNone);
  EXPECT_FALSE(hold.Holding());
}

// B6
TEST(SessionDemandHold, ClearDropsTheHold) {
  Hold hold(kSessionDemandHoldMs);
  const std::uint64_t ticket = hold.Begin(kNowMs);
  hold.Clear();
  EXPECT_FALSE(hold.Holding());
  EXPECT_EQ(hold.Finish(ticket, true), Step::kNone);
}

// B7
TEST(SessionDemandHold, HoldIsNotHoldingBeforeBegin) {
  Hold hold(kSessionDemandHoldMs);
  EXPECT_EQ(hold.Poll(kNowMs), Step::kNone);
  EXPECT_EQ(hold.Finish(kNowMs + 1, true), Step::kNone);
  EXPECT_FALSE(hold.Holding());
}

// B8
TEST(SessionLevelOfSettings, ReadsEveryForm) {
  using nevr_game_type_table::LevelOfSettings;
  EXPECT_EQ(LevelOfSettings("{\"level\":5764607523034234880}"), kLevelDecimal);
  EXPECT_EQ(LevelOfSettings("{\"level\":-1}"), kNegativeOneBits);
  EXPECT_EQ(LevelOfSettings("{\"level\":\"mpl_arena_a\"}"), kMplArenaAHash);
  EXPECT_FALSE(LevelOfSettings("{}").has_value());
  EXPECT_FALSE(LevelOfSettings("{\"level\":0}").has_value());
  EXPECT_FALSE(LevelOfSettings("{\"level\":\"\"}").has_value());
  EXPECT_FALSE(LevelOfSettings("{\"level\":true}").has_value());
  EXPECT_FALSE(LevelOfSettings("{\"level\":[]}").has_value());
}

// B9
TEST(SessionDemandSettings, ReadsGameTypeAndLevelFromTheFrame) {
  using nevr_game_type_table::DemandSettingsOfSessionStart;
  const std::string json = "{\"gametype\":\"echo_combat\",\"level\":\"mpl_arena_a\"}";
  const std::vector<std::uint8_t> frame = FrameWith(json, true);
  const auto settings = DemandSettingsOfSessionStart(frame.data(), frame.size());
  ASSERT_TRUE(settings.has_value());
  EXPECT_EQ(settings->gameType, "echo_combat");
  EXPECT_EQ(settings->level, kMplArenaAHash);

  const std::vector<std::uint8_t> shortFrame(nevr_game_type_table::kSessionStartHeaderSize - 1, 0);
  EXPECT_FALSE(DemandSettingsOfSessionStart(shortFrame.data(), shortFrame.size()).has_value());

  const std::vector<std::uint8_t> unterminated = FrameWith(json, false);
  EXPECT_FALSE(DemandSettingsOfSessionStart(unterminated.data(), unterminated.size()).has_value());
}

// B10 (F1): only the held session's own end drops its start. A late CODE_ENDED that names an earlier session, or a game-side end of
// another session, leaves the hold standing; the decision is EndsHeldSession, which OnSessionEnd calls before it clears anything.
TEST(SessionDemandEnd, OnlyTheHeldSessionsOwnEndDropsIt) {
  constexpr std::string_view kHeld = "13121110-1514-1716-1819-1A1B1C1D1E1F";
  constexpr std::string_view kHeldLowerCase = "13121110-1514-1716-1819-1a1b1c1d1e1f";
  constexpr std::string_view kEarlier = "00112233-4455-6677-8899-AABBCCDDEEFF";
  constexpr std::string_view kOneDigitShort = "13121110-1514-1716-1819-1A1B1C1D1E1";
  EXPECT_TRUE(nevr_session_demand::EndsHeldSession(kHeld, kHeld));
  EXPECT_TRUE(nevr_session_demand::EndsHeldSession(kHeld, kHeldLowerCase));
  EXPECT_FALSE(nevr_session_demand::EndsHeldSession(kHeld, kEarlier));
  EXPECT_FALSE(nevr_session_demand::EndsHeldSession(kHeld, kOneDigitShort));
  EXPECT_FALSE(nevr_session_demand::EndsHeldSession(kHeld, ""));
  EXPECT_FALSE(nevr_session_demand::EndsHeldSession("", ""));
  EXPECT_FALSE(nevr_session_demand::EndsHeldSession("none", "none"));
}

// B12 (N3): a Begin replaces a hold only while one stands. The second start of a held session reports the replacement (the log line
// that names both sessions and the dropped accepts); a Begin after the hold ended, by an answer, the deadline or Clear, does not.
TEST(SessionDemandHold, OnlyABeginOverAStandingHoldReplacesIt) {
  Hold hold(kSessionDemandHoldMs);
  hold.Begin(kNowMs);
  EXPECT_FALSE(hold.Replaced());
  hold.Begin(kNowMs);
  EXPECT_TRUE(hold.Replaced());

  const std::uint64_t ticket = hold.Ticket();
  EXPECT_EQ(hold.Finish(ticket, true), Step::kForward);
  hold.Begin(kNowMs);
  EXPECT_FALSE(hold.Replaced());

  hold.Clear();
  hold.Begin(kNowMs);
  EXPECT_FALSE(hold.Replaced());

  hold.Begin(kNowMs);
  EXPECT_TRUE(hold.Replaced());
  EXPECT_EQ(hold.Poll(kSessionDemandHoldMs), Step::kFail);
  hold.Begin(kNowMs);
  EXPECT_FALSE(hold.Replaced());
}

// B11
TEST(SessionDemandHold, TheConstantsAreTheNamedSum) {
  EXPECT_EQ(kSessionDemandHoldMs, kNamedHoldSumMs);
  EXPECT_EQ(kDemandGraceMs, kSessionDemandHoldMs - nevr_cdn::kServerDemandMs);
}
