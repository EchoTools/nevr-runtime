// #2242 (EchoCombat nevr-pack 82ad7ba): PAYLOAD SPEED and the payload PAUSE. The decisions are runtime/combat/payload_speed_rules.h;
// the stub site, the clock and the mailbox are veilnet.cpp's.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "runtime/combat/payload_speed_rules.h"

namespace {

namespace ps = nevr_combat::payload_speed;

// EchoCombat's own numbers, stated here and not read from the header, so a change to a constant fails below instead of matching itself.
constexpr float kGameSpeed = 8.0f;
constexpr float kStanding = 0.0f;
constexpr std::uint8_t kHalf = 2, kX1 = 4, kDouble = 8, kQuarterX = 1, kTripleX = 12, kTwoAndHalf = 10, kUnset = 0;
constexpr std::uint32_t kCatPaused = 1, kCatAutodrive = 2, kCatLocked = 8, kNoFlags = 0;
constexpr std::size_t kCatFlagsOffset = 0x30;
constexpr unsigned kCatMapShift = 5, kCatSpeedShift = 16;
constexpr std::uint32_t kEverything = 0xFFFFFFFFu, kMapBit = 1u << kCatMapShift;
constexpr std::size_t kRowCap = 64;
// What the quarters mean as a multiplier of the game's speed.
constexpr float kTimesTwo = 2.0f, kTimesThree = 3.0f, kTimesTwoAndHalf = 2.5f, kTimesHalf = 0.5f, kTimesQuarter = 0.25f;
constexpr std::uint8_t kMaxByte = 0xFF;
constexpr float kForward = 0.06f, kBarely = 0.05f;

TEST(PayloadSpeedRules, TheValuesAreEchoCombatsOwn) {
  EXPECT_EQ(ps::kFlagPaused, kCatPaused);
  EXPECT_EQ(ps::kFlagAutodrive, kCatAutodrive);
  EXPECT_EQ(ps::kFlagLocked, kCatLocked);
  EXPECT_EQ(ps::kFlagsOffset, kCatFlagsOffset);
  EXPECT_EQ(ps::kMapShift, kCatMapShift);
  EXPECT_EQ(ps::kSpeedShift, kCatSpeedShift);
}

TEST(PayloadSpeedRules, QuartersScaleTheGamesSpeed) {
  EXPECT_FLOAT_EQ(ps::Scaled(kGameSpeed, kX1), kGameSpeed);
  EXPECT_FLOAT_EQ(ps::Scaled(kGameSpeed, kUnset), kGameSpeed);
  EXPECT_FLOAT_EQ(ps::Scaled(kGameSpeed, kDouble), kGameSpeed * kTimesTwo);
  EXPECT_FLOAT_EQ(ps::Scaled(kGameSpeed, kHalf), kGameSpeed * kTimesHalf);
  EXPECT_FLOAT_EQ(ps::Scaled(kGameSpeed, kQuarterX), kGameSpeed * kTimesQuarter);
  EXPECT_FLOAT_EQ(ps::Scaled(kGameSpeed, kTwoAndHalf), kGameSpeed * kTimesTwoAndHalf);
}

TEST(PayloadSpeedRules, ThePauseHoldsThePayloadEvenOnAutodrive) {
  EXPECT_FLOAT_EQ(ps::ServerStep(kNoFlags, true, kX1, kGameSpeed).speed, kStanding);
  EXPECT_FLOAT_EQ(ps::ServerStep(kCatAutodrive, true, kX1, kGameSpeed).speed, kStanding);
  EXPECT_FLOAT_EQ(ps::ClientSpeed(kNoFlags, true, kDouble, kGameSpeed), kStanding);
  EXPECT_FLOAT_EQ(ps::ClientSpeed(kCatAutodrive, true, kDouble, kGameSpeed), kStanding);
}

TEST(PayloadSpeedRules, AutodriveIsTheGamesSpeedAndStartsTheBookkeepingOver) {
  const ps::Step s = ps::ServerStep(kCatAutodrive, false, kDouble, kGameSpeed);
  EXPECT_FLOAT_EQ(s.speed, kGameSpeed);
  EXPECT_EQ(s.moved, ps::Moved::kClear);
  EXPECT_FLOAT_EQ(ps::ClientSpeed(kCatAutodrive, false, kDouble, kGameSpeed), kGameSpeed);
}

TEST(PayloadSpeedRules, AForwardSpeedMarksThePayloadAsMovedAndIsScaled) {
  const ps::Step s = ps::ServerStep(kNoFlags, false, kDouble, kGameSpeed);
  EXPECT_FLOAT_EQ(s.speed, kGameSpeed * kTimesTwo);
  EXPECT_EQ(s.moved, ps::Moved::kSet);
  EXPECT_FLOAT_EQ(ps::ClientSpeed(kNoFlags, false, kTripleX, kGameSpeed), kGameSpeed * kTimesThree);
}

TEST(PayloadSpeedRules, ForwardIsStrictlyAboveTheThreshold) {
  EXPECT_EQ(ps::ServerStep(kNoFlags, false, kX1, kForward).moved, ps::Moved::kSet);
  EXPECT_EQ(ps::ServerStep(kNoFlags, false, kX1, kBarely).moved, ps::Moved::kKeep);
  EXPECT_EQ(ps::ServerStep(kNoFlags, false, kX1, kStanding).moved, ps::Moved::kKeep);
}

TEST(PayloadSpeedRules, LockedEndsTheRoundUnlessThePayloadStillMovesForward) {
  EXPECT_EQ(ps::ServerStep(kCatLocked, false, kX1, kStanding).moved, ps::Moved::kClear);
  EXPECT_EQ(ps::ServerStep(kCatLocked, true, kX1, kGameSpeed).moved, ps::Moved::kClear);
  EXPECT_EQ(ps::ServerStep(kCatLocked, false, kX1, kGameSpeed).moved, ps::Moved::kSet);
}

TEST(PayloadSpeedRules, APauseOnAMovingPayloadWaitsForTheCheckpointHold) {
  EXPECT_TRUE(ps::PauseWaitsForCheckpoint(true, true, kNoFlags));
  EXPECT_FALSE(ps::PauseWaitsForCheckpoint(true, true, kCatPaused));   // already held: pause at once
  EXPECT_FALSE(ps::PauseWaitsForCheckpoint(true, false, kNoFlags));    // before the attackers start
  EXPECT_FALSE(ps::PauseWaitsForCheckpoint(false, true, kNoFlags));    // not a payload map
}

TEST(PayloadSpeedRules, TheRowIsPayloadSpeedOnAPayloadMapAndPointRateElsewhere) {
  char t[kRowCap];
  ps::RowText(t, sizeof(t), true, kDouble, kHalf);
  EXPECT_STREQ(t, "PAYLOAD SPEED: 2.0x");
  ps::RowText(t, sizeof(t), false, kDouble, kHalf);
  EXPECT_STREQ(t, "POINT RATE: 0.5x");
  ps::RowText(t, sizeof(t), true, kUnset, kHalf);
  EXPECT_STREQ(t, "PAYLOAD SPEED: 1.0x");
  ps::RowText(t, sizeof(t), true, kTripleX, kHalf);
  EXPECT_STREQ(t, "PAYLOAD SPEED: 3x");
  ps::RowText(t, sizeof(t), true, kQuarterX, kHalf);
  EXPECT_STREQ(t, "PAYLOAD SPEED: 0.25x");
  ps::RowText(t, sizeof(t), true, kTwoAndHalf, kHalf);
  EXPECT_STREQ(t, "PAYLOAD SPEED: 2.5x");
}

TEST(PayloadSpeedRules, TheGWordCarriesTheMapBitAndTheQuarters) {
  const std::uint32_t w = ps::PackWord(true, kTwoAndHalf);
  EXPECT_EQ(w, kMapBit | static_cast<std::uint32_t>(kTwoAndHalf) << kCatSpeedShift);
  EXPECT_TRUE(ps::WordMap(w));
  EXPECT_EQ(ps::WordQuarters(w), kTwoAndHalf);
  EXPECT_FALSE(ps::WordMap(ps::PackWord(false, kX1)));
  EXPECT_FALSE(ps::WordMap(kEverything & ~kMapBit));
  EXPECT_EQ(ps::WordQuarters(kEverything), kMaxByte);
}

}  // namespace
