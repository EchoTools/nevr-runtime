/* SYNTHESIS -- custom tool code, not from binary */

// The events the telemetry stream finds between two snapshots (#2099): the goal's team from the game's
// last_score.team (0 blue, anything else orange: echovr.exe 0x140155f43), a goal for every rise of a team's
// score (a scorer who scores twice in a row included; the record's distance and assist fields change during
// play, so a change of the record is no goal), a round start after the frames the
// game names no state for (its state-to-string function 0x14016d510 returns "" between a score and the next
// round start), joins, leaves, switches and stat rises on the game slot (a slot another account took is a
// leave and a join; a stat that falls is no event), and nothing for a snapshot the game thread could not read.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

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
  d.lastScore = b.lastScore;              // slot 5 again: the same scorer, team and points
  const auto frames = DetectOver({a, b, c, d});
  EXPECT_EQ(Count(frames[0], EchoEvent::kGoalScored), 1);
  EXPECT_EQ(Count(frames[1], EchoEvent::kGoalScored), 0);
  ASSERT_EQ(Count(frames[2], EchoEvent::kGoalScored), 1);
  const auto& g = First(frames[2], EchoEvent::kGoalScored)->goal_scored();
  EXPECT_EQ(g.scorer_slot(), 5);
  EXPECT_FALSE(g.has_assist_slot());
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
