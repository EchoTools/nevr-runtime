/* SYNTHESIS -- custom tool code, not from binary */

// The events the telemetry stream finds between two snapshots (#2099): a goal for every rise of a team's
// score, with that team and the rise as its points (a scorer who scores twice in a row included; the record's
// distance and assist fields change during play, so a change of the record is no goal); the scorer, assist,
// disc speed and distance from the last-score record only when the record names that team (last_score.team:
// 0 blue, anything else orange, echovr.exe 0x140155f43) and is not the record an earlier goal took, else the
// goal waits up to 3 read frames for it and goes out without them; a round start after the frames the
// game names no state for (its state-to-string function 0x14016d510 returns "" between a score and the next
// round start), joins, leaves, switches and stat rises on the game slot (a slot another account took is a
// leave and a join; a stat that falls is no event), and nothing for a snapshot the game thread could not read.
// Round starts and ends carry the game's round_counter, a pause or unpause the pause state as /session names it
// (#2100), and a join the player's jersey number and level; a change of either is a player_info_updated.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/server/telemetry_events.h"

namespace {

using telemetry::v2::EchoArenaFrame;
using telemetry::v2::EchoEvent;

// A snapshot the game thread read, with nothing in it.
TelemetrySnapshot Empty() {
  TelemetrySnapshot s;
  s.Clear();
  s.gameRead = true;
  return s;
}

TelemetrySnapshot WithStatus(uint64_t symbol) {
  TelemetrySnapshot s = Empty();
  s.gameStatusHash = symbol;
  return s;
}

TelemetrySnapshot::LastScore Goal(int16_t team, uint32_t scorer, float distance) {
  TelemetrySnapshot::LastScore g{};
  g.team = team;
  g.pointAmount = 2;
  g.discSpeed = 11.5f;
  g.distanceThrown = distance;
  g.assistPacked = 0x1F;  // no assist
  g.scorerPacked = scorer;
  return g;
}

int Count(const EchoArenaFrame& f, EchoEvent::EventCase c) {
  int n = 0;
  for (const auto& e : f.events()) n += e.event_case() == c ? 1 : 0;
  return n;
}

const EchoEvent* First(const EchoArenaFrame& f, EchoEvent::EventCase c) {
  for (const auto& e : f.events()) {
    if (e.event_case() == c) return &e;
  }
  return nullptr;
}

// Runs detection over a sequence of snapshots, one frame per step after the first, and returns the frames.
std::vector<EchoArenaFrame> DetectOver(const std::vector<TelemetrySnapshot>& snaps) {
  TelemetryEventState state;
  std::vector<EchoArenaFrame> frames;
  for (size_t i = 1; i < snaps.size(); i++) {
    frames.emplace_back();
    DetectTelemetryEvents(snaps[i], snaps[i - 1], state, &frames.back());
  }
  return frames;
}

constexpr uint64_t kPlaying = nevr_game_status_hash::PLAYING_1;
constexpr uint64_t kScore = nevr_game_status_hash::SCORE_1;
constexpr uint64_t kRoundStart = nevr_game_status_hash::ROUND_START_1;
constexpr uint64_t kRoundOver = nevr_game_status_hash::ROUND_OVER_1;
constexpr uint64_t kPostMatch = nevr_game_status_hash::POST_MATCH_1;
constexpr uint64_t kNoName = 0;  // a state the game's state-to-string function has no name for

TEST(TelemetryEvents, ARoundStartAfterAnUnnamedStateIsSent) {
  const auto frames = DetectOver({WithStatus(kPlaying), WithStatus(kScore), WithStatus(kNoName), WithStatus(kRoundStart)});
  EXPECT_EQ(Count(frames[2], EchoEvent::kRoundStarted), 1);
  int total = 0;
  for (const auto& f : frames) total += Count(f, EchoEvent::kRoundStarted);
  EXPECT_EQ(total, 1);
}

TEST(TelemetryEvents, ARoundStartDirectlyAfterAnotherStateIsSent) {
  const auto frames = DetectOver({WithStatus(kPlaying), WithStatus(kPlaying), WithStatus(kRoundStart)});
  EXPECT_EQ(Count(frames[1], EchoEvent::kRoundStarted), 1);
}

TEST(TelemetryEvents, AnUnnamedStateBetweenTwoRoundStartsSendsNoSecond) {
  const auto frames = DetectOver({WithStatus(kScore), WithStatus(kRoundStart), WithStatus(kNoName), WithStatus(kRoundStart)});
  EXPECT_EQ(Count(frames[0], EchoEvent::kRoundStarted), 1);
  EXPECT_EQ(Count(frames[2], EchoEvent::kRoundStarted), 0);
}

TEST(TelemetryEvents, TheFirstStatusOfASessionSendsNothing) {
  const auto frames = DetectOver({Empty(), WithStatus(kRoundStart), WithStatus(kRoundStart)});
  EXPECT_EQ(Count(frames[0], EchoEvent::kRoundStarted), 0);
  EXPECT_EQ(Count(frames[1], EchoEvent::kRoundStarted), 0);
}

TEST(TelemetryEvents, RoundOverAndPostMatchAfterAnUnnamedStateAreSent) {
  const auto frames = DetectOver({WithStatus(kPlaying), WithStatus(kNoName), WithStatus(kRoundOver), WithStatus(kNoName),
                           WithStatus(kPostMatch)});
  EXPECT_EQ(Count(frames[1], EchoEvent::kRoundEnded), 1);
  EXPECT_EQ(Count(frames[3], EchoEvent::kMatchEnded), 1);
  EXPECT_EQ(Count(frames[1], EchoEvent::kRoundStarted), 0);
}

TEST(TelemetryEvents, ARoundStartAndEndCarryTheGamesRoundNumber) {
  TelemetrySnapshot a = WithStatus(kPlaying), b = WithStatus(kRoundStart), c = WithStatus(kPlaying),
                    d = WithStatus(kRoundOver);
  b.roundNumber = 2;
  d.roundNumber = 3;
  const auto frames = DetectOver({a, b, c, d});
  ASSERT_NE(First(frames[0], EchoEvent::kRoundStarted), nullptr);
  EXPECT_EQ(First(frames[0], EchoEvent::kRoundStarted)->round_started().round_number(), 2);
  ASSERT_NE(First(frames[2], EchoEvent::kRoundEnded), nullptr);
  EXPECT_EQ(First(frames[2], EchoEvent::kRoundEnded)->round_ended().round_number(), 3);
}

TelemetrySnapshot WithPause(const char* state, float time) {
  TelemetrySnapshot s = WithStatus(kPlaying);
  s.pauseStateSymbol = EchoVR::CSymbol64Hash(state);
  s.pauseTime = time;
  return s;
}

TEST(TelemetryEvents, APauseIsSentWhenTheGameBecomesPausedAndAnUnpauseWhenItLeaves) {
  const auto frames = DetectOver({WithPause("unpaused", 0.0f), WithPause("pause_requested", 0.0f),
                                  WithPause("paused", 0.0f), WithPause("paused", 2.0f),
                                  WithPause("unpaused", 1.5f), WithPause("unpaused", 0.0f)});
  EXPECT_EQ(Count(frames[0], EchoEvent::kRoundPaused), 0);  // a request is not a pause
  ASSERT_NE(First(frames[1], EchoEvent::kRoundPaused), nullptr);
  EXPECT_EQ(First(frames[1], EchoEvent::kRoundPaused)->round_paused().pause_state(), telemetry::v2::PAUSE_STATE_PAUSED);
  EXPECT_EQ(frames[2].events_size(), 0);  // still paused
  ASSERT_NE(First(frames[3], EchoEvent::kRoundUnpaused), nullptr);
  EXPECT_EQ(First(frames[3], EchoEvent::kRoundUnpaused)->round_unpaused().pause_state(),
            telemetry::v2::PAUSE_STATE_UNPAUSING);
  EXPECT_EQ(frames[4].events_size(), 0);  // unpausing to not paused is neither
  int pauses = 0, unpauses = 0;
  for (const auto& f : frames) {
    pauses += Count(f, EchoEvent::kRoundPaused);
    unpauses += Count(f, EchoEvent::kRoundUnpaused);
  }
  EXPECT_EQ(pauses, 1);
  EXPECT_EQ(unpauses, 1);
}

TEST(TelemetryEvents, AnUnpauseStraightToUnpausedCarriesNotPaused) {
  const auto frames = DetectOver({WithPause("paused", 0.0f), WithPause("unpaused", 0.0f)});
  ASSERT_NE(First(frames[0], EchoEvent::kRoundUnpaused), nullptr);
  EXPECT_EQ(First(frames[0], EchoEvent::kRoundUnpaused)->round_unpaused().pause_state(),
            telemetry::v2::PAUSE_STATE_NOT_PAUSED);
}

TEST(TelemetryEvents, AGoalsTeamIsBlueForZeroAndOrangeOtherwise) {
  TelemetrySnapshot a = Empty(), b = Empty(), c = Empty();
  b.bluePoints = 2;
  b.lastScore = Goal(0, 3, 10.0f);
  c.bluePoints = 2;
  c.orangePoints = 3;
  c.lastScore = Goal(1, 4, 12.0f);
  const auto frames = DetectOver({a, b, c});
  ASSERT_NE(First(frames[0], EchoEvent::kGoalScored), nullptr);
  EXPECT_EQ(First(frames[0], EchoEvent::kGoalScored)->goal_scored().team(), telemetry::v2::ROLE_BLUE_TEAM);
  ASSERT_NE(First(frames[1], EchoEvent::kGoalScored), nullptr);
  EXPECT_EQ(First(frames[1], EchoEvent::kGoalScored)->goal_scored().team(), telemetry::v2::ROLE_ORANGE_TEAM);
  EXPECT_EQ(RoleFromLastScoreTeam(2), telemetry::v2::ROLE_ORANGE_TEAM);
  EXPECT_EQ(RoleFromLastScoreTeam(-1), telemetry::v2::ROLE_ORANGE_TEAM);
}

TEST(TelemetryEvents, AScoreRiseSendsOneGoalWithTheRecordsFields) {
  TelemetrySnapshot a = Empty(), b = Empty();
  b.orangePoints = 2;
  b.lastScore = Goal(1, 0x0405, 19.8f);  // scorer slot 5
  b.lastScore.assistPacked = 0x0802;     // assist slot 2
  const auto frames = DetectOver({a, b, b});
  ASSERT_EQ(Count(frames[0], EchoEvent::kGoalScored), 1);
  const auto& g = First(frames[0], EchoEvent::kGoalScored)->goal_scored();
  EXPECT_EQ(g.team(), telemetry::v2::ROLE_ORANGE_TEAM);
  EXPECT_EQ(g.point_amount(), 2);
  EXPECT_EQ(g.scorer_slot(), 5);
  EXPECT_EQ(g.assist_slot(), 2);
  EXPECT_FLOAT_EQ(g.distance_thrown(), 19.8f);
  EXPECT_FLOAT_EQ(g.disc_speed(), 11.5f);
  EXPECT_EQ(Count(frames[1], EchoEvent::kGoalScored), 0);
}

TEST(TelemetryEvents, TwoGoalsInARowByTheSamePlayerAreTwoEvents) {
  TelemetrySnapshot a = Empty(), b = Empty(), c = Empty(), d = Empty();
  b.bluePoints = 2;
  b.lastScore = Goal(0, 0x0405, 19.8f);  // scorer slot 5
  c = b;                                  // nothing new
  d.bluePoints = 4;
  d.lastScore = b.lastScore;              // slot 5 again: the same scorer, team and points,
  d.lastScore.discSpeed = 13.25f;         // in a record of its own
  const auto frames = DetectOver({a, b, c, d});
  EXPECT_EQ(Count(frames[0], EchoEvent::kGoalScored), 1);
  EXPECT_EQ(Count(frames[1], EchoEvent::kGoalScored), 0);
  ASSERT_EQ(Count(frames[2], EchoEvent::kGoalScored), 1);
  const auto& g = First(frames[2], EchoEvent::kGoalScored)->goal_scored();
  EXPECT_EQ(g.scorer_slot(), 5);
  EXPECT_FALSE(g.has_assist_slot());
  EXPECT_FLOAT_EQ(g.disc_speed(), 13.25f);
  EXPECT_EQ(g.point_amount(), 2);
}

// The goal events of a frame, in order.
std::vector<telemetry::v2::GoalScored> Goals(const EchoArenaFrame& f) {
  std::vector<telemetry::v2::GoalScored> goals;
  for (const auto& e : f.events()) {
    if (e.event_case() == EchoEvent::kGoalScored) goals.push_back(e.goal_scored());
  }
  return goals;
}

// A goal sent without the record: its team and points, and no scorer, assist, speed or distance.
void ExpectNoRecordFields(const telemetry::v2::GoalScored& g) {
  EXPECT_EQ(g.scorer_slot(), 0);
  EXPECT_FALSE(g.has_assist_slot());
  EXPECT_EQ(g.disc_speed(), 0.0f);
  EXPECT_EQ(g.distance_thrown(), 0.0f);
}

TEST(TelemetryEvents, AGoalWaitsWhileTheRecordNamesTheOtherTeamAndGoesOutWhenItCatchesUp) {
  TelemetrySnapshot a = Empty();
  a.bluePoints = 2;
  a.lastScore = Goal(0, 0x0403, 7.0f);  // the last goal was blue's
  TelemetrySnapshot b = a;
  b.orangePoints = 3;                   // orange scores; the record is still blue's
  TelemetrySnapshot c = b;
  c.lastScore = Goal(1, 0x0409, 21.5f);  // the record catches up: orange, slot 9
  c.lastScore.pointAmount = 3;
  const auto frames = DetectOver({a, b, c, c});
  EXPECT_EQ(Count(frames[0], EchoEvent::kGoalScored), 0);
  EXPECT_EQ(Count(frames[0], EchoEvent::kScoreboardUpdated), 1);
  const auto goals = Goals(frames[1]);
  ASSERT_EQ(goals.size(), 1u);
  EXPECT_EQ(goals[0].team(), telemetry::v2::ROLE_ORANGE_TEAM);
  EXPECT_EQ(goals[0].point_amount(), 3);
  EXPECT_EQ(goals[0].scorer_slot(), 9);
  EXPECT_FLOAT_EQ(goals[0].distance_thrown(), 21.5f);
  EXPECT_EQ(Count(frames[2], EchoEvent::kGoalScored), 0);
}

TEST(TelemetryEvents, AGoalWhoseRecordNeverComesGoesOutAfterThreeFramesWithoutItsFields) {
  TelemetrySnapshot a = Empty();
  a.lastScore = Goal(0, 0x0403, 7.0f);  // blue's
  TelemetrySnapshot b = a;
  b.orangePoints = 2;
  const auto frames = DetectOver({a, b, b, b, b, b});
  EXPECT_EQ(Count(frames[0], EchoEvent::kGoalScored), 0);
  EXPECT_EQ(Count(frames[1], EchoEvent::kGoalScored), 0);
  EXPECT_EQ(Count(frames[2], EchoEvent::kGoalScored), 0);
  const auto goals = Goals(frames[3]);
  ASSERT_EQ(goals.size(), 1u);
  EXPECT_EQ(goals[0].team(), telemetry::v2::ROLE_ORANGE_TEAM);
  EXPECT_EQ(goals[0].point_amount(), 2);
  ExpectNoRecordFields(goals[0]);
  EXPECT_EQ(Count(frames[4], EchoEvent::kGoalScored), 0);
}

TEST(TelemetryEvents, BothScoresRisingInOneSnapshotAreTwoGoalsAndOnlyTheRecordsTeamGetsItsFields) {
  TelemetrySnapshot a = Empty(), b = Empty();
  b.bluePoints = 2;
  b.orangePoints = 3;
  b.lastScore = Goal(1, 0x0406, 15.0f);  // the record is orange's
  const auto frames = DetectOver({a, b, b, b, b});
  auto goals = Goals(frames[0]);
  ASSERT_EQ(goals.size(), 1u);
  EXPECT_EQ(goals[0].team(), telemetry::v2::ROLE_ORANGE_TEAM);
  EXPECT_EQ(goals[0].point_amount(), 3);
  EXPECT_EQ(goals[0].scorer_slot(), 6);
  EXPECT_FLOAT_EQ(goals[0].distance_thrown(), 15.0f);
  EXPECT_EQ(Count(frames[1], EchoEvent::kGoalScored), 0);
  EXPECT_EQ(Count(frames[2], EchoEvent::kGoalScored), 0);
  goals = Goals(frames[3]);  // blue's never got a record of its own
  ASSERT_EQ(goals.size(), 1u);
  EXPECT_EQ(goals[0].team(), telemetry::v2::ROLE_BLUE_TEAM);
  EXPECT_EQ(goals[0].point_amount(), 2);
  ExpectNoRecordFields(goals[0]);
}

TEST(TelemetryEvents, ASecondRiseWithTheFirstGoalsRecordUnchangedWaits) {
  TelemetrySnapshot a = Empty(), b = Empty();
  b.bluePoints = 2;
  b.lastScore = Goal(0, 0x0405, 19.8f);
  TelemetrySnapshot c = b;
  c.bluePoints = 4;                      // blue again; the record is still the first goal's
  TelemetrySnapshot d = c;
  d.lastScore.discSpeed = 9.75f;         // its own record arrives
  d.lastScore.distanceThrown = 3.0f;
  const auto frames = DetectOver({a, b, c, d});
  EXPECT_EQ(Count(frames[0], EchoEvent::kGoalScored), 1);
  EXPECT_EQ(Count(frames[1], EchoEvent::kGoalScored), 0);
  const auto goals = Goals(frames[2]);
  ASSERT_EQ(goals.size(), 1u);
  EXPECT_EQ(goals[0].team(), telemetry::v2::ROLE_BLUE_TEAM);
  EXPECT_EQ(goals[0].point_amount(), 2);
  EXPECT_FLOAT_EQ(goals[0].disc_speed(), 9.75f);
  EXPECT_FLOAT_EQ(goals[0].distance_thrown(), 3.0f);
}

TEST(TelemetryEvents, ARepeatGoalWhoseRecordLooksLikeTheLastOneGetsItsFieldsAfterTheWait) {
  // Measured on a live match (echovr.exe 34.4.631547.1): the record's disc speed is the same value in every
  // record, so the same player scoring the same points twice in a row for one team writes a record that equals
  // the last goal's in team, points, speed and scorer. Only its distance tells it apart.
  TelemetrySnapshot a = Empty(), b = Empty(), c = Empty();
  b.orangePoints = 3;
  b.lastScore = Goal(1, 0x0407, 10.67f);  // slot 7, 3 points
  b.lastScore.pointAmount = 3;
  b.lastScore.discSpeed = -204845580000.0f;
  c = b;
  c.orangePoints = 6;                     // slot 7 again, 3 points again
  c.lastScore.distanceThrown = 26.89f;
  const auto frames = DetectOver({a, b, c, c, c, c, c});
  ASSERT_EQ(Count(frames[0], EchoEvent::kGoalScored), 1);
  EXPECT_EQ(Count(frames[1], EchoEvent::kGoalScored), 0);
  EXPECT_EQ(Count(frames[2], EchoEvent::kGoalScored), 0);
  EXPECT_EQ(Count(frames[3], EchoEvent::kGoalScored), 0);
  const auto goals = Goals(frames[4]);
  ASSERT_EQ(goals.size(), 1u);
  EXPECT_EQ(goals[0].team(), telemetry::v2::ROLE_ORANGE_TEAM);
  EXPECT_EQ(goals[0].point_amount(), 3);
  EXPECT_EQ(goals[0].scorer_slot(), 7);
  EXPECT_FLOAT_EQ(goals[0].distance_thrown(), 26.89f);
  EXPECT_EQ(Count(frames[5], EchoEvent::kGoalScored), 0);
}

TEST(TelemetryEvents, AfterTheWaitARecordOfThisTeamWithOtherPointsStillGivesNoFields) {
  TelemetrySnapshot a = Empty(), b = Empty(), c = Empty();
  b.bluePoints = 2;
  b.lastScore = Goal(0, 0x0405, 19.8f);  // blue, 2 points
  c = b;
  c.bluePoints = 5;                      // blue rises by 3; the record stays the 2-point goal's
  const auto frames = DetectOver({a, b, c, c, c, c});
  ASSERT_EQ(Count(frames[0], EchoEvent::kGoalScored), 1);
  const auto goals = Goals(frames[4]);
  ASSERT_EQ(goals.size(), 1u);
  EXPECT_EQ(goals[0].team(), telemetry::v2::ROLE_BLUE_TEAM);
  EXPECT_EQ(goals[0].point_amount(), 3);
  ExpectNoRecordFields(goals[0]);
}

TEST(TelemetryEvents, AFrameThatWasNotReadIsNotAFrameAGoalWaited) {
  TelemetrySnapshot a = Empty();
  a.lastScore = Goal(0, 0x0403, 7.0f);
  TelemetrySnapshot b = a;
  b.orangePoints = 2;
  TelemetrySnapshot unread;
  unread.Clear();
  // As the streamer calls it: an unread snapshot against the last read one, which stays `prev`.
  TelemetryEventState state;
  std::vector<EchoArenaFrame> frames(6);
  DetectTelemetryEvents(b, a, state, &frames[0]);       // the rise
  DetectTelemetryEvents(unread, b, state, &frames[1]);
  DetectTelemetryEvents(unread, b, state, &frames[2]);
  DetectTelemetryEvents(unread, b, state, &frames[3]);
  DetectTelemetryEvents(b, b, state, &frames[4]);        // read frames 1 and 2 after the rise
  DetectTelemetryEvents(b, b, state, &frames[5]);
  for (const auto& f : frames) EXPECT_EQ(Count(f, EchoEvent::kGoalScored), 0);
  EchoArenaFrame third;
  DetectTelemetryEvents(b, b, state, &third);
  EXPECT_EQ(Count(third, EchoEvent::kGoalScored), 1);
}

TEST(TelemetryEvents, ARecordThatChangesDuringPlayIsNoGoal) {
  // Measured on a live match: after a goal the record keeps team 0, points 2 and scorer 3 while distance and
  // assist change many times a second.
  TelemetrySnapshot a = Empty(), b = Empty(), c = Empty();
  a.bluePoints = b.bluePoints = c.bluePoints = 2;
  a.lastScore = Goal(0, 3, 0.0f);
  b.lastScore = Goal(0, 3, 4.931f);
  b.lastScore.assistPacked = 0x0001;
  c.lastScore = Goal(0, 3, 8.033f);
  const auto frames = DetectOver({a, b, c});
  EXPECT_EQ(Count(frames[0], EchoEvent::kGoalScored), 0);
  EXPECT_EQ(Count(frames[1], EchoEvent::kGoalScored), 0);
}

TEST(TelemetryEvents, ARoundCountTheGameSetsAfterTheHeaderGoesOutAsMatchSettingsChanged) {
  TelemetrySnapshot a = Empty(), b = Empty(), c = Empty();
  a.totalRoundCount = 0;  // the session's first snapshots (the header's)
  b.totalRoundCount = 3;  // pre_match: the game sets the count
  c.totalRoundCount = 3;
  const auto frames = DetectOver({a, b, c});
  ASSERT_EQ(Count(frames[0], EchoEvent::kMatchSettingsChanged), 1);
  const auto& m = First(frames[0], EchoEvent::kMatchSettingsChanged)->match_settings_changed();
  EXPECT_EQ(m.total_round_count(), 3);
  EXPECT_TRUE(m.rules_changed_by().empty());
  EXPECT_EQ(m.rules_changed_at(), 0u);
  EXPECT_EQ(Count(frames[1], EchoEvent::kMatchSettingsChanged), 0);
}

TEST(TelemetryEvents, AnUnreadSnapshotIsNoRoundCountChange) {
  TelemetrySnapshot a = Empty();
  a.totalRoundCount = 3;
  TelemetrySnapshot unread;
  unread.Clear();
  TelemetryEventState state;
  EchoArenaFrame f1, f2;
  DetectTelemetryEvents(unread, a, state, &f1);
  DetectTelemetryEvents(a, a, state, &f2);
  EXPECT_EQ(Count(f1, EchoEvent::kMatchSettingsChanged), 0);
  EXPECT_EQ(Count(f2, EchoEvent::kMatchSettingsChanged), 0);
}

TEST(TelemetryEvents, AScoreThatFallsIsNoGoal) {
  TelemetrySnapshot a = Empty(), b = Empty();
  a.bluePoints = 4;
  a.orangePoints = 6;
  b.lastScore = Goal(0, 3, 1.0f);  // a new match: the scores go back to 0
  const auto frames = DetectOver({a, b});
  EXPECT_EQ(Count(frames[0], EchoEvent::kGoalScored), 0);
  EXPECT_EQ(Count(frames[0], EchoEvent::kScoreboardUpdated), 1);
}

TEST(TelemetryEvents, AScoreChangeSendsTheScoreboard) {
  TelemetrySnapshot a = Empty(), b = Empty(), c = Empty();
  b.bluePoints = 3;
  c.bluePoints = 3;
  c.orangePoints = 2;
  const auto frames = DetectOver({a, b, b, c});
  ASSERT_NE(First(frames[0], EchoEvent::kScoreboardUpdated), nullptr);
  EXPECT_EQ(First(frames[0], EchoEvent::kScoreboardUpdated)->scoreboard_updated().blue_points(), 3);
  EXPECT_EQ(Count(frames[1], EchoEvent::kScoreboardUpdated), 0);
  ASSERT_NE(First(frames[2], EchoEvent::kScoreboardUpdated), nullptr);
  EXPECT_EQ(First(frames[2], EchoEvent::kScoreboardUpdated)->scoreboard_updated().orange_points(), 2);
  EXPECT_EQ(First(frames[2], EchoEvent::kScoreboardUpdated)->scoreboard_updated().blue_points(), 3);
}

TelemetrySnapshot WithPlayer(uint8_t slot, uint16_t team, uint64_t id, const char* name) {
  TelemetrySnapshot s = Empty();
  s.playerInfo[slot].present = true;
  s.playerInfo[slot].teamIndex = team;
  s.playerInfo[slot].accountId = id;
  std::strncpy(s.playerInfo[slot].displayName, name, sizeof(s.playerInfo[slot].displayName) - 1);
  s.playerCount = 1;
  return s;
}

TEST(TelemetryEvents, AJoinCarriesTheSlotRoleIdAndName) {
  const auto frames = DetectOver({Empty(), WithPlayer(9, 1, 0xABCDEF, "orange_nine")});
  ASSERT_NE(First(frames[0], EchoEvent::kPlayerJoined), nullptr);
  const auto& j = First(frames[0], EchoEvent::kPlayerJoined)->player_joined();
  EXPECT_EQ(j.slot(), 9);
  EXPECT_EQ(j.role(), telemetry::v2::ROLE_ORANGE_TEAM);
  EXPECT_EQ(j.account_number(), 0xABCDEFu);
  EXPECT_EQ(j.display_name(), "orange_nine");
}

TEST(TelemetryEvents, AJoinCarriesTheJerseyNumberAndLevel) {
  TelemetrySnapshot b = WithPlayer(9, 1, 0xABCDEF, "orange_nine");
  b.playerInfo[9].jerseyNumber = 21;
  b.playerInfo[9].level = 40;
  const auto frames = DetectOver({Empty(), b});
  ASSERT_NE(First(frames[0], EchoEvent::kPlayerJoined), nullptr);
  EXPECT_EQ(First(frames[0], EchoEvent::kPlayerJoined)->player_joined().jersey_number(), 21);
  EXPECT_EQ(First(frames[0], EchoEvent::kPlayerJoined)->player_joined().level(), 40);
  EXPECT_EQ(Count(frames[0], EchoEvent::kPlayerInfoUpdated), 0);
}

TEST(TelemetryEvents, AJerseyOrLevelChangeOfTheSamePlayerIsAnInfoUpdate) {
  TelemetrySnapshot a = WithPlayer(5, 0, 7, "blue5"), b = a, c = a, d = a;
  a.playerInfo[5].jerseyNumber = 3;
  a.playerInfo[5].level = 10;
  b.playerInfo[5].jerseyNumber = 4;  // jersey changes
  b.playerInfo[5].level = 10;
  c.playerInfo[5].jerseyNumber = 4;
  c.playerInfo[5].level = 11;  // level changes
  d.playerInfo[5].jerseyNumber = 4;
  d.playerInfo[5].level = 11;  // nothing changes
  const auto frames = DetectOver({a, b, c, d});
  ASSERT_NE(First(frames[0], EchoEvent::kPlayerInfoUpdated), nullptr);
  const auto& u0 = First(frames[0], EchoEvent::kPlayerInfoUpdated)->player_info_updated();
  EXPECT_EQ(u0.player_slot(), 5);
  EXPECT_EQ(u0.jersey_number(), 4);
  EXPECT_EQ(u0.level(), 10);
  ASSERT_NE(First(frames[1], EchoEvent::kPlayerInfoUpdated), nullptr);
  EXPECT_EQ(First(frames[1], EchoEvent::kPlayerInfoUpdated)->player_info_updated().level(), 11);
  EXPECT_EQ(Count(frames[2], EchoEvent::kPlayerInfoUpdated), 0);
}

TEST(TelemetryEvents, ASlotTakenByAnotherAccountWithAnotherJerseyIsNoInfoUpdate) {
  TelemetrySnapshot a = WithPlayer(5, 0, 7, "blue5"), b = WithPlayer(5, 0, 8, "other");
  a.playerInfo[5].jerseyNumber = 3;
  b.playerInfo[5].jerseyNumber = 9;
  const auto frames = DetectOver({a, b});
  EXPECT_EQ(Count(frames[0], EchoEvent::kPlayerInfoUpdated), 0);
  EXPECT_EQ(Count(frames[0], EchoEvent::kPlayerJoined), 1);
}

TEST(TelemetryEvents, ALeaveCarriesTheSlot) {
  const auto frames = DetectOver({WithPlayer(11, 0, 1, "x"), Empty()});
  ASSERT_NE(First(frames[0], EchoEvent::kPlayerLeft), nullptr);
  EXPECT_EQ(First(frames[0], EchoEvent::kPlayerLeft)->player_left().player_slot(), 11);
  EXPECT_EQ(Count(frames[0], EchoEvent::kPlayerJoined), 0);
}

TEST(TelemetryEvents, ATeamSwitchCarriesTheNewAndPreviousRole) {
  const auto frames = DetectOver({WithPlayer(4, 0, 1, "x"), WithPlayer(4, 2, 1, "x")});
  ASSERT_NE(First(frames[0], EchoEvent::kPlayerSwitchedTeam), nullptr);
  const auto& sw = First(frames[0], EchoEvent::kPlayerSwitchedTeam)->player_switched_team();
  EXPECT_EQ(sw.player_slot(), 4);
  EXPECT_EQ(sw.new_role(), telemetry::v2::ROLE_SPECTATOR);
  EXPECT_EQ(sw.prev_role(), telemetry::v2::ROLE_BLUE_TEAM);
  EXPECT_EQ(Count(frames[0], EchoEvent::kPlayerJoined), 0);
}

TEST(TelemetryEvents, AStatRiseIsSentOnTheGameSlotWithItsTotal) {
  TelemetrySnapshot a = WithPlayer(13, 0, 1, "x");
  TelemetrySnapshot b = a;
  a.stats[13].stuns = 1;
  b.stats[13].stuns = 2;
  b.stats[13].blocks = 1;
  b.stats[13].saves = 4;
  b.stats[13].interceptions = 1;
  b.stats[13].steals = 1;
  b.stats[13].passes = 1;
  b.stats[13].shotsTaken = 1;
  const auto frames = DetectOver({a, b, b});
  const auto& f = frames[0];
  ASSERT_NE(First(f, EchoEvent::kPlayerStun), nullptr);
  EXPECT_EQ(First(f, EchoEvent::kPlayerStun)->player_stun().player_slot(), 13);
  EXPECT_EQ(First(f, EchoEvent::kPlayerStun)->player_stun().total_stuns(), 2);
  ASSERT_NE(First(f, EchoEvent::kPlayerBlock), nullptr);
  EXPECT_EQ(First(f, EchoEvent::kPlayerBlock)->player_block().player_slot(), 13);
  EXPECT_EQ(First(f, EchoEvent::kPlayerBlock)->player_block().total_blocks(), 1);
  ASSERT_NE(First(f, EchoEvent::kPlayerSave), nullptr);
  EXPECT_EQ(First(f, EchoEvent::kPlayerSave)->player_save().player_slot(), 13);
  EXPECT_EQ(First(f, EchoEvent::kPlayerSave)->player_save().total_saves(), 4);
  ASSERT_NE(First(f, EchoEvent::kPlayerInterception), nullptr);
  EXPECT_EQ(First(f, EchoEvent::kPlayerInterception)->player_interception().player_slot(), 13);
  ASSERT_NE(First(f, EchoEvent::kPlayerSteal), nullptr);
  EXPECT_EQ(First(f, EchoEvent::kPlayerSteal)->player_steal().player_slot(), 13);
  ASSERT_NE(First(f, EchoEvent::kPlayerPass), nullptr);
  EXPECT_EQ(First(f, EchoEvent::kPlayerPass)->player_pass().player_slot(), 13);
  ASSERT_NE(First(f, EchoEvent::kPlayerShotTaken), nullptr);
  EXPECT_EQ(First(f, EchoEvent::kPlayerShotTaken)->player_shot_taken().player_slot(), 13);
  EXPECT_EQ(frames[1].events_size(), 0);
}

TEST(TelemetryEvents, AStatOfAPlayerWhoJustJoinedIsNoEvent) {
  TelemetrySnapshot b = WithPlayer(2, 0, 1, "x");
  b.stats[2].stuns = 3;
  const auto frames = DetectOver({Empty(), b});
  EXPECT_EQ(Count(frames[0], EchoEvent::kPlayerStun), 0);
}

TEST(TelemetryEvents, AStatThatFallsSendsNoEvent) {
  // A new match puts every stat back to 0; only a rise is an event.
  TelemetrySnapshot a = WithPlayer(13, 0, 1, "x");
  TelemetrySnapshot b = a;
  auto& s = a.stats[13];
  s.stuns = s.blocks = s.saves = s.interceptions = s.steals = s.passes = s.shotsTaken = 5;
  s.points = s.goals = s.assists = s.catches = 5;
  s.possessionTime = 5.0f;
  auto& t = b.stats[13];
  t.stuns = t.blocks = t.saves = t.interceptions = t.steals = t.passes = t.shotsTaken = 3;
  const auto frames = DetectOver({a, b});
  EXPECT_EQ(frames[0].events_size(), 0);
}

TEST(TelemetryEvents, ASlotTakenByAnotherAccountIsALeaveAndAJoin) {
  TelemetrySnapshot a = WithPlayer(4, 0, 0x111, "old");
  TelemetrySnapshot b = WithPlayer(4, 1, 0x222, "new");
  a.stats[4].stuns = 1;
  b.stats[4].stuns = 5;  // the new player's own count, not a rise of the old one's
  const auto frames = DetectOver({a, b});
  const auto& f = frames[0];
  ASSERT_EQ(f.events_size(), 2);
  EXPECT_EQ(f.events(0).event_case(), EchoEvent::kPlayerLeft);
  EXPECT_EQ(f.events(0).player_left().player_slot(), 4);
  ASSERT_EQ(f.events(1).event_case(), EchoEvent::kPlayerJoined);
  EXPECT_EQ(f.events(1).player_joined().slot(), 4);
  EXPECT_EQ(f.events(1).player_joined().account_number(), 0x222u);
  EXPECT_EQ(f.events(1).player_joined().display_name(), "new");
  EXPECT_EQ(f.events(1).player_joined().role(), telemetry::v2::ROLE_ORANGE_TEAM);
}

TEST(TelemetryEvents, ASnapshotThatWasNotReadIsNeitherSentNorComparedWith) {
  TelemetrySnapshot read = WithPlayer(3, 0, 7, "x");
  read.bluePoints = 2;
  read.orangePoints = 1;
  read.gameStatusHash = kPlaying;
  TelemetrySnapshot unread;
  unread.Clear();  // what SnapshotIfDue publishes when SnapshotGameState could not resolve the game
  const auto frames = DetectOver({read, unread, read});
  EXPECT_EQ(frames[0].events_size(), 0);  // no leave, no scoreboard
  EXPECT_EQ(frames[1].events_size(), 0);  // no join, no goal from 0, no status change
}

TEST(TelemetryEvents, ASlotWithNoPlayerSendsNoStatEvent) {
  TelemetrySnapshot a = Empty(), b = Empty();  // slot 6 holds no player in either
  a.stats[6].stuns = 1;
  b.stats[6].stuns = 2;
  b.playerInfo[6].teamIndex = 1;
  const auto frames = DetectOver({a, b});
  EXPECT_EQ(frames[0].events_size(), 0);
}

}  // namespace
