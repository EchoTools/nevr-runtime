// The new-unlocks clear that must survive an update in flight (#2410): the pure rules in patch/new_unlocks_clear_rules.h. The game's own
// clear sets the profile's dirty bit, but a change made while an update is unacknowledged is merged into that update and its dirty bit is
// cleared without a send, and the acknowledgement then discards the merge (the scenario clear_while_update_in_flight shows it). The rules
// decide when a clear is held and when the dirty bit is set again (a poll once per frame); the glue (patch/new_unlocks_clear.cpp on PC) reads
// the game and applies it.

#include <gtest/gtest.h>

#include "runtime/patch/new_unlocks_clear_rules.h"

namespace rules = nevr_new_unlocks_clear;

namespace {

// Two distinct objects (not const: equal constants may share one address): the tests compare these as user pointers.
int kUserA = 0;
int kUserB = 0;

TEST(NewUnlocksClearRules, ClearWithNothingInFlightIsLeftToTheGame) {
  rules::Hold hold;
  EXPECT_EQ(hold.OnClear(&kUserA, 3, /*inFlight=*/false), rules::ClearVerdict::kSentNormally);
  EXPECT_FALSE(hold.Holds(&kUserA));
  EXPECT_EQ(hold.OnPoll(&kUserA, false, 0), rules::PollVerdict::kNothingHeld);
}

TEST(NewUnlocksClearRules, ClearOfAnEmptyListChangedNothing) {
  rules::Hold hold;
  // The game's clear returns at once when the count is 0: there is no change to lose, even with an update in flight.
  EXPECT_EQ(hold.OnClear(&kUserA, 0, /*inFlight=*/true), rules::ClearVerdict::kNothingCleared);
  EXPECT_FALSE(hold.Holds(&kUserA));
}

TEST(NewUnlocksClearRules, ClearInFlightIsHeldAndRearmedOnceAfterTheAcknowledgement) {
  rules::Hold hold;
  EXPECT_EQ(hold.OnClear(&kUserA, 5, /*inFlight=*/true), rules::ClearVerdict::kHeld);
  EXPECT_TRUE(hold.Holds(&kUserA));
  EXPECT_EQ(hold.HeldCount(), 5u);
  // Still unacknowledged: keep holding, change nothing.
  EXPECT_EQ(hold.OnPoll(&kUserA, /*inFlight=*/true, 0), rules::PollVerdict::kStillInFlight);
  EXPECT_TRUE(hold.Holds(&kUserA));
  // The acknowledgement arrived: set the dirty bit again, once.
  EXPECT_EQ(hold.OnPoll(&kUserA, /*inFlight=*/false, 0), rules::PollVerdict::kRearm);
  EXPECT_FALSE(hold.Holds(&kUserA));
  EXPECT_EQ(hold.LastCount(), 5u);  // the value the re-armed line logs
  EXPECT_EQ(hold.OnPoll(&kUserA, false, 0), rules::PollVerdict::kNothingHeld);
}

TEST(NewUnlocksClearRules, APollForAnotherUserNeverRearms) {
  rules::Hold hold;
  ASSERT_EQ(hold.OnClear(&kUserA, 2, true), rules::ClearVerdict::kHeld);
  EXPECT_EQ(hold.OnPoll(&kUserB, false, 0), rules::PollVerdict::kNothingHeld);
  EXPECT_TRUE(hold.Holds(&kUserA));
}

TEST(NewUnlocksClearRules, ANewClearReplacesAHeldOneAndAClearWithNothingInFlightDropsIt) {
  rules::Hold hold;
  ASSERT_EQ(hold.OnClear(&kUserA, 4, true), rules::ClearVerdict::kHeld);
  // The same list cleared again with nothing in flight: the game sends this one itself, so nothing is left to re-arm.
  EXPECT_EQ(hold.OnClear(&kUserA, 1, false), rules::ClearVerdict::kSentNormally);
  EXPECT_FALSE(hold.Holds(&kUserA));
  ASSERT_EQ(hold.OnClear(&kUserB, 7, true), rules::ClearVerdict::kHeld);
  EXPECT_EQ(hold.HeldCount(), 7u);
  EXPECT_TRUE(hold.Holds(&kUserB));
  EXPECT_FALSE(hold.Holds(&kUserA));
}

TEST(NewUnlocksClearRules, AnAddAfterTheClearSupersedesIt) {
  rules::Hold hold;
  ASSERT_EQ(hold.OnClear(&kUserA, 4, true), rules::ClearVerdict::kHeld);
  // The list is not empty again at the next poll: sending an empty update would wipe what the add put back.
  EXPECT_EQ(hold.OnPoll(&kUserA, /*inFlight=*/true, /*countNow=*/1), rules::PollVerdict::kSuperseded);
  EXPECT_FALSE(hold.Holds(&kUserA));
  EXPECT_EQ(hold.OnPoll(&kUserA, /*inFlight=*/false, /*countNow=*/1), rules::PollVerdict::kNothingHeld);
}

TEST(NewUnlocksClearRules, TheHeldUserIsTheGateAndADroppedClearIsNeverRearmed) {
  rules::Hold hold;
  EXPECT_EQ(hold.HeldUser(), nullptr);
  ASSERT_EQ(hold.OnClear(&kUserA, 3, true), rules::ClearVerdict::kHeld);
  EXPECT_EQ(hold.HeldUser(), &kUserA);
  hold.Drop();
  EXPECT_EQ(hold.HeldUser(), nullptr);
  EXPECT_FALSE(hold.Holds(&kUserA));
  EXPECT_EQ(hold.HeldCount(), 0u);
  EXPECT_EQ(hold.OnPoll(&kUserA, false, 0), rules::PollVerdict::kNothingHeld);
}

TEST(NewUnlocksClearRules, TheGameOffsetsAreTheDocumentedOnes) {
  EXPECT_EQ(rules::kNetGameUserOffset, 0x28D0u);
  EXPECT_EQ(rules::kNewUnlocksCountOffset, 0x51C50u);
  EXPECT_EQ(rules::kUserInFlightOffset, 0x28u);
  EXPECT_EQ(rules::kUserFlagsOffset, 0x9Cu);
  EXPECT_EQ(rules::kUserDirtyBit, 0x1u);
}

}  // namespace
