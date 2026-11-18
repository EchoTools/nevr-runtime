// Tests for runtime/server/telemetry_events.h: the events a telemetry frame carries that come from comparing the frame's snapshot with
// the previous one, and the game's status symbol to the telemetry status (telemetry_game_status.h).
#include "runtime/server/telemetry_events.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

namespace {

namespace h = nevr_game_status_hash;
using telemetry::v2::EchoArenaFrame;

constexpr std::uint64_t kAccount = 4242;
constexpr std::uint16_t kSlot = 3;
constexpr std::uint16_t kOtherSlot = 5;
constexpr std::uint32_t kNoAssist = nevr_telemetry::kNoAssistSlot;
constexpr std::uint16_t kTeamBlue = 0;
constexpr std::uint16_t kTeamOrange = 1;
constexpr std::uint16_t kPointsThree = 3;
constexpr std::int32_t kBlue = 2;
constexpr std::int32_t kOrange = 1;
constexpr std::uint32_t kStuns = 2;
constexpr std::uint32_t kBlocks = 3;
constexpr std::uint32_t kSaves = 4;
constexpr std::uint32_t kGoalsNotReported = 9;
constexpr std::uint32_t kStunsBefore = 5;
constexpr std::uint32_t kStunsAfterReset = 4;
constexpr int kStatEvents = 7;
constexpr int kSaveAt = 2;  // the stat events come in the order the counters are compared: stun, block, save, interception, steal, pass, shot
constexpr int kInterceptionAt = 3;
constexpr int kStealAt = 4;
constexpr int kPassAt = 5;
constexpr int kShotAt = 6;
constexpr std::uint32_t kScorerPacked = (nevr_telemetry::kSlotMask + 1) | kSlot;  // a bit above the slot bits: only the low ones are the slot

TelemetrySnapshot Empty() {
  TelemetrySnapshot s;
  s.Clear();
  s.gameStatusHash = h::PLAYING_1;
  return s;
}

void Seat(TelemetrySnapshot* s, std::uint16_t slot) {
  s->playerInfo[slot].accountId = kAccount + slot;
  std::strncpy(s->playerInfo[slot].displayName, "p", sizeof(s->playerInfo[slot].displayName) - 1);
  if (s->playerCount <= slot) s->playerCount = static_cast<std::uint16_t>(slot + 1);
}

EchoArenaFrame Detect(const TelemetrySnapshot& curr, const TelemetrySnapshot& prev) {
  EchoArenaFrame frame;
  nevr_telemetry::DetectEvents(curr, prev, &frame);
  return frame;
}

}  // namespace

TEST(TelemetryStatus, EachGroupOfTheGamesStateSymbolsMapsToItsStatusAndUnknownsToUnspecified) {
  using telemetry::v2::GameStatus;
  EXPECT_EQ(GameStatusFromSymbol(h::PRE_MATCH_3), telemetry::v2::GAME_STATUS_PRE_MATCH);
  EXPECT_EQ(GameStatusFromSymbol(h::ROUND_START_5), telemetry::v2::GAME_STATUS_ROUND_START);
  EXPECT_EQ(GameStatusFromSymbol(h::PLAYING_2), telemetry::v2::GAME_STATUS_PLAYING);
  EXPECT_EQ(GameStatusFromSymbol(h::SCORE_1), telemetry::v2::GAME_STATUS_SCORE);
  EXPECT_EQ(GameStatusFromSymbol(h::ROUND_OVER_2), telemetry::v2::GAME_STATUS_ROUND_OVER);
  EXPECT_EQ(GameStatusFromSymbol(h::POST_MATCH_5), telemetry::v2::GAME_STATUS_POST_MATCH);
  EXPECT_EQ(GameStatusFromSymbol(h::PRE_SUDDEN_DEATH_2), telemetry::v2::GAME_STATUS_PRE_SUDDEN_DEATH);
  EXPECT_EQ(GameStatusFromSymbol(h::SUDDEN_DEATH_1), telemetry::v2::GAME_STATUS_SUDDEN_DEATH);
  EXPECT_EQ(GameStatusFromSymbol(h::POST_SUDDEN_DEATH_2), telemetry::v2::GAME_STATUS_POST_SUDDEN_DEATH);
  EXPECT_EQ(GameStatusFromSymbol(0), telemetry::v2::GAME_STATUS_UNSPECIFIED);
  EXPECT_EQ(GameStatusFromSymbol(h::PLAYING_1 + 1), telemetry::v2::GAME_STATUS_UNSPECIFIED);
}

TEST(TelemetryEvents, NothingChangedMeansNoEvents) {
  TelemetrySnapshot a = Empty();
  Seat(&a, kSlot);
  EXPECT_EQ(Detect(a, a).events_size(), 0);
}

TEST(TelemetryEvents, AStatusChangeNamesRoundStartRoundEndAndMatchEndOnly) {
  TelemetrySnapshot prev = Empty();
  TelemetrySnapshot curr = Empty();
  curr.gameStatusHash = h::ROUND_START_1;
  EchoArenaFrame f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), 1);
  EXPECT_TRUE(f.events(0).has_round_started());
  curr.gameStatusHash = h::ROUND_OVER_1;
  f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), 1);
  EXPECT_TRUE(f.events(0).has_round_ended());
  curr.gameStatusHash = h::POST_MATCH_1;
  f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), 1);
  EXPECT_TRUE(f.events(0).has_match_ended());
  curr.gameStatusHash = h::SCORE_1;
  EXPECT_EQ(Detect(curr, prev).events_size(), 0) << "a status with no event of its own";
}

TEST(TelemetryEvents, NoStatusEventFromAnUnknownPreviousStatusOrWithinTheSameGroup) {
  TelemetrySnapshot prev = Empty();
  prev.gameStatusHash = 0;  // not a status the game names
  TelemetrySnapshot curr = Empty();
  curr.gameStatusHash = h::ROUND_START_1;
  EXPECT_EQ(Detect(curr, prev).events_size(), 0) << "the first frame after an unknown status is not a transition";
  prev.gameStatusHash = h::ROUND_START_2;
  EXPECT_EQ(Detect(curr, prev).events_size(), 0) << "two symbols of one status are not a change";
}

TEST(TelemetryEvents, AScoreChangeOnEitherSideCarriesBothScores) {
  TelemetrySnapshot prev = Empty();
  TelemetrySnapshot curr = Empty();
  curr.bluePoints = kBlue;
  curr.orangePoints = kOrange;
  EchoArenaFrame f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), 1);
  ASSERT_TRUE(f.events(0).has_scoreboard_updated());
  EXPECT_EQ(f.events(0).scoreboard_updated().blue_points(), kBlue);
  EXPECT_EQ(f.events(0).scoreboard_updated().orange_points(), kOrange);
  prev.bluePoints = kBlue;
  f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), 1) << "orange alone changed";
  prev.orangePoints = 1;
  EXPECT_EQ(Detect(curr, prev).events_size(), 0);
}

TEST(TelemetryEvents, AGoalIsANewScorerWithPointsAndNamesItsScorerAndAssist) {
  TelemetrySnapshot prev = Empty();
  TelemetrySnapshot curr = Empty();
  curr.lastScore.scorerPacked = kScorerPacked;
  curr.lastScore.assistPacked = kOtherSlot;
  curr.lastScore.pointAmount = kPointsThree;
  curr.lastScore.team = kTeamOrange;
  EchoArenaFrame f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), 1);
  ASSERT_TRUE(f.events(0).has_goal_scored());
  const auto& goal = f.events(0).goal_scored();
  EXPECT_EQ(goal.scorer_slot(), kSlot);
  EXPECT_EQ(goal.assist_slot(), kOtherSlot);
  EXPECT_EQ(goal.point_amount(), kPointsThree);
  EXPECT_EQ(static_cast<int>(goal.team()), static_cast<int>(kTeamOrange));

  curr.lastScore.assistPacked = kNoAssist;
  f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), 1);
  EXPECT_EQ(f.events(0).goal_scored().assist_slot(), 0u) << "no assist is the all-ones slot: the field is left unset";

  curr.lastScore.pointAmount = 0;
  EXPECT_EQ(Detect(curr, prev).events_size(), 0) << "a new scorer with no points is not a goal";
  curr.lastScore.pointAmount = kPointsThree;
  prev.lastScore.scorerPacked = kScorerPacked;
  EXPECT_EQ(Detect(curr, prev).events_size(), 0) << "the same last score is not a new goal";
}

TEST(TelemetryEvents, APlayerJoiningAndLeavingAreEventsWithTheirSlot) {
  TelemetrySnapshot prev = Empty();
  TelemetrySnapshot curr = Empty();
  Seat(&curr, kSlot);
  EchoArenaFrame f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), 1);
  ASSERT_TRUE(f.events(0).has_player_joined());
  EXPECT_EQ(f.events(0).player_joined().slot(), kSlot);
  EXPECT_EQ(f.events(0).player_joined().account_number(), kAccount + kSlot);
  EXPECT_EQ(f.events(0).player_joined().display_name(), "p");
  f = Detect(prev, curr);
  ASSERT_EQ(f.events_size(), 1);
  ASSERT_TRUE(f.events(0).has_player_left());
  EXPECT_EQ(f.events(0).player_left().player_slot(), kSlot);
}

TEST(TelemetryEvents, ASlotWithNoAccountIsNotPresent) {
  TelemetrySnapshot prev = Empty();
  TelemetrySnapshot curr = Empty();
  curr.playerCount = kSlot + 1;  // counted but its account id is 0
  EXPECT_EQ(Detect(curr, prev).events_size(), 0);
  Seat(&curr, kSlot);
  curr.playerCount = kSlot;  // the account is there but past the player count
  EXPECT_EQ(Detect(curr, prev).events_size(), 0);
}

TEST(TelemetryEvents, ATeamSwitchOfAPlayerPresentBeforeIsAnEventWithTheNewRole) {
  TelemetrySnapshot prev = Empty();
  Seat(&prev, kSlot);
  prev.playerInfo[kSlot].teamIndex = kTeamBlue;
  TelemetrySnapshot curr = prev;
  curr.playerInfo[kSlot].teamIndex = kTeamOrange;
  const EchoArenaFrame f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), 1);
  ASSERT_TRUE(f.events(0).has_player_switched_team());
  EXPECT_EQ(f.events(0).player_switched_team().player_slot(), kSlot);
  EXPECT_EQ(static_cast<int>(f.events(0).player_switched_team().new_role()), static_cast<int>(kTeamOrange));
}

TEST(TelemetryEvents, EachStatCounterThatRoseIsItsOwnEventAndOneThatDidNotIsNone) {
  TelemetrySnapshot prev = Empty();
  Seat(&prev, kSlot);
  TelemetrySnapshot curr = prev;
  auto& s = curr.stats[kSlot];
  s.stuns = kStuns;
  s.blocks = kBlocks;
  s.saves = kSaves;
  s.interceptions = 1;
  s.steals = 1;
  s.passes = 1;
  s.shotsTaken = 1;
  s.goals = kGoalsNotReported;  // not a counter this frame reports
  const EchoArenaFrame f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), kStatEvents);
  EXPECT_TRUE(f.events(0).has_player_stun());
  EXPECT_EQ(f.events(0).player_stun().total_stuns(), kStuns);
  EXPECT_TRUE(f.events(1).has_player_block());
  EXPECT_EQ(f.events(1).player_block().total_blocks(), kBlocks);
  EXPECT_TRUE(f.events(kSaveAt).has_player_save());
  EXPECT_EQ(f.events(kSaveAt).player_save().total_saves(), kSaves);
  EXPECT_TRUE(f.events(kInterceptionAt).has_player_interception());
  EXPECT_TRUE(f.events(kStealAt).has_player_steal());
  EXPECT_TRUE(f.events(kPassAt).has_player_pass());
  EXPECT_TRUE(f.events(kShotAt).has_player_shot_taken());
  EXPECT_EQ(f.events(kInterceptionAt).player_interception().player_slot(), kSlot);
}

TEST(TelemetryEvents, ACounterThatFellOrStayedIsNoEvent) {
  TelemetrySnapshot prev = Empty();
  Seat(&prev, kSlot);
  prev.stats[kSlot].stuns = kStunsBefore;
  TelemetrySnapshot curr = prev;
  curr.stats[kSlot].stuns = kStunsAfterReset;  // a new match resets the counters
  EXPECT_EQ(Detect(curr, prev).events_size(), 0);
}

TEST(TelemetryEvents, AJoinersCountersAreNotComparedWithAnEmptySlot) {
  TelemetrySnapshot prev = Empty();
  TelemetrySnapshot curr = Empty();
  Seat(&curr, kSlot);
  curr.stats[kSlot].stuns = 1;  // joined this frame: its counters are not compared with a slot that was empty
  const EchoArenaFrame f = Detect(curr, prev);
  ASSERT_EQ(f.events_size(), 1);
  EXPECT_TRUE(f.events(0).has_player_joined());
}
