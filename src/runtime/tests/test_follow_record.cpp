// The party-follow records (#1413): the proceed run and the decision tracker that the JoinPartyLobby and CanFollowParty
// hooks use to decide what to write (patch/follow_record_rules.h). Pure: no game, no clock, no log call. The hooks
// themselves are not driven here; this file covers the decisions they take.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include "runtime/compat/lobby_frame.h"
#include "runtime/patch/follow_record_rules.h"

namespace {

using nevr_follow_record::DecisionState;
using nevr_follow_record::LobbyPrefix;
using nevr_follow_record::NoteDecision;
using nevr_follow_record::NoteNoProceed;
using nevr_follow_record::NoteProceed;
using nevr_follow_record::ProceedLogState;
using Guid = std::array<uint8_t, 16>;

Guid MakeGuid(uint8_t seed) {
  Guid g{};
  for (std::size_t i = 0; i < g.size(); ++i) g[i] = static_cast<uint8_t>(seed + i);
  return g;
}

const Guid kNil{};
const Guid kX = MakeGuid(0x10);
const Guid kY = MakeGuid(0x90);

TEST(FollowRecordProceed, FirstProceedWritesWithNoRepeats) {
  ProceedLogState state;
  const auto d = NoteProceed(state, kX);
  EXPECT_TRUE(d.writeInfo);
  EXPECT_EQ(d.repeats, 0u);
}

TEST(FollowRecordProceed, RepeatsOfTheSameLobbyAreCountedNotWritten) {
  ProceedLogState state;
  ASSERT_TRUE(NoteProceed(state, kX).writeInfo);
  for (int i = 0; i < 5; ++i) EXPECT_FALSE(NoteProceed(state, kX).writeInfo) << "repeat " << i;
  // A different lobby writes again and carries the five repeats it followed.
  const auto next = NoteProceed(state, kY);
  EXPECT_TRUE(next.writeInfo);
  EXPECT_EQ(next.repeats, 5u);
}

TEST(FollowRecordProceed, ASkipOrHoldEndsTheRunSoTheSameLobbyWritesAgain) {
  ProceedLogState state;
  ASSERT_TRUE(NoteProceed(state, kX).writeInfo);
  ASSERT_FALSE(NoteProceed(state, kX).writeInfo);
  NoteNoProceed(state);
  const auto again = NoteProceed(state, kX);
  EXPECT_TRUE(again.writeInfo);
  EXPECT_EQ(again.repeats, 1u);  // the one repeat before the skip is carried, not lost
}

TEST(FollowRecordProceed, AnUnknownLobbyIsItsOwnRun) {
  ProceedLogState state;
  ASSERT_TRUE(NoteProceed(state, kNil).writeInfo);
  EXPECT_FALSE(NoteProceed(state, kNil).writeInfo);
  EXPECT_TRUE(NoteProceed(state, kX).writeInfo);
}

TEST(FollowRecordDecision, FirstAnswerIsWritten) {
  DecisionState state;
  EXPECT_TRUE(NoteDecision(state, true, 5, 0));
}

TEST(FollowRecordDecision, AnUnchangedAnswerIsNotWritten) {
  DecisionState state;
  ASSERT_TRUE(NoteDecision(state, true, 5, 0));
  for (int i = 0; i < 100; ++i) EXPECT_FALSE(NoteDecision(state, true, 5, 0)) << "frame " << i;
}

TEST(FollowRecordDecision, EachInputChangeIsWritten) {
  DecisionState state;
  ASSERT_TRUE(NoteDecision(state, false, 5, 3));
  EXPECT_TRUE(NoteDecision(state, true, 5, 3)) << "the answer changed";
  EXPECT_TRUE(NoteDecision(state, true, 9, 3)) << "the game state changed";
  EXPECT_TRUE(NoteDecision(state, true, 9, 0)) << "the lobby status changed";
  EXPECT_FALSE(NoteDecision(state, true, 9, 0));
}

TEST(FollowRecordDecision, AnUnreadLobbyStatusIsItsOwnValue) {
  DecisionState state;
  ASSERT_TRUE(NoteDecision(state, false, 2, -1));
  EXPECT_FALSE(NoteDecision(state, false, 2, -1));
  EXPECT_TRUE(NoteDecision(state, false, 2, 0)) << "a read status differs from the unread one";
}

TEST(FollowRecordPrefix, NilLobbyIsNone) { EXPECT_EQ(LobbyPrefix(kNil), "none"); }

TEST(FollowRecordPrefix, KnownLobbyIsTheFirstEightCharactersOfItsGuid) {
  const std::string full = nevr_lobby_frame::FormatGuid(kX);
  ASSERT_GT(full.size(), 8u);
  EXPECT_EQ(LobbyPrefix(kX), full.substr(0, 8));
  EXPECT_EQ(LobbyPrefix(kX).size(), 8u);
  EXPECT_NE(LobbyPrefix(kX), LobbyPrefix(kY));
}

}  // namespace
