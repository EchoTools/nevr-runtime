// #2242 (EchoCombat 2.0.3): a new session starts with the default RULES, and VeilNet's watcher reacts once per new session,
// never on the first one the process sees. The mailbox writes are the header's (runtime/combat/rules_state.h), which the
// game's bytes are read through; these tests drive them on a byte array of the same shape.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "runtime/combat/match_control_rules.h"
#include "runtime/combat/rules_state.h"

namespace {

namespace rules = nevr_combat::rules;
namespace mc = nevr_combat::match_control;

// The default is x1 (4 quarters, runtime/combat/rules_state.h); the test states it itself, so a change to the header's constant
// fails here instead of matching itself.
constexpr std::uint8_t kPointRateX1 = 4;

// The mailbox the test writes (a block of the same shape), the byte it is filled with, and the bytes a new session must not touch.
constexpr std::size_t kMailboxBytes = 0x40;
constexpr std::uint8_t kFill = 0xAB;
constexpr std::array<std::uint32_t, 9> kUntouchedBytes = {0x10u, 0x12u, 0x13u, 0x14u, 0x16u, 0x2Fu, 0x30u, 0x32u, 0x33u};

float FloatAt(const std::array<std::uint8_t, kMailboxBytes>& m, std::uint32_t at) {
  float f = 0.0f;
  std::memcpy(&f, m.data() + at, sizeof f);
  return f;
}

TEST(VeilnetRules, DefaultsWriteTheFloatsAndTheShownBytesAFreshSessionStartsWith) {
  std::array<std::uint8_t, kMailboxBytes> m{};
  m.fill(kFill);
  rules::ApplyDefaults(m.data());
  for (std::uint32_t at : {rules::kBlueStartScore, rules::kOrangeStartScore, rules::kRoundTotal, rules::kCaptureTime})
    EXPECT_EQ(FloatAt(m, at), 0.0f) << "float at +" << at;
  EXPECT_EQ(m[rules::kClassicShown], rules::kLoadoutAll);
  EXPECT_EQ(m[rules::kChassisShown], 0);
  EXPECT_EQ(m[rules::kPointRate], kPointRateX1);
  EXPECT_EQ(m[rules::kPspeedShown], kPointRateX1);
  EXPECT_EQ(m[rules::kPausedShown], 0);
  EXPECT_EQ(m[rules::kPauseQueuedShown], 0);
}

TEST(VeilnetRules, DefaultsLeaveTheOtherMailboxBytesAlone) {
  std::array<std::uint8_t, kMailboxBytes> m{};
  m.fill(kFill);
  rules::ApplyDefaults(m.data());
  for (std::uint32_t at : kUntouchedBytes) EXPECT_EQ(m[at], kFill) << "byte +" << at;
}

TEST(VeilnetRules, TheFirstSessionTheProcessSeesIsNotANewOne) {
  std::uint64_t seen = 0;
  EXPECT_FALSE(rules::NewSessionSeen(seen, 1));
  EXPECT_EQ(seen, 1u);
}

TEST(VeilnetRules, NoSessionYetChangesNothing) {
  std::uint64_t seen = 0;
  EXPECT_FALSE(rules::NewSessionSeen(seen, 0));
  EXPECT_EQ(seen, 0u);
}

TEST(VeilnetRules, EverySessionAfterTheFirstIsNewOnce) {
  std::uint64_t seen = 1;
  EXPECT_TRUE(rules::NewSessionSeen(seen, 2));
  EXPECT_EQ(seen, 2u);
  EXPECT_FALSE(rules::NewSessionSeen(seen, 2));
  EXPECT_TRUE(rules::NewSessionSeen(seen, 3));
}

// ---- the server's 'G' word and a client's writes (#2242, runtime/combat/match_control_rules.h, shared with the Quest) ----

// The bytes a client writes from 'G', stated here (veilnet.cpp's 'G' branch), so a moved offset in the header fails.
constexpr std::uint32_t kLoadoutsAt = 0x11, kChassisAt = 0x2C, kRateAt = 0x2D, kPauseAt = 0x2E, kPayloadMapAt = 0x30, kSpeedAt = 0x31,
                        kCheckpointAt = 0x32, kSkipAt = 0x33, kQueuedAt = 0x34, kRoundTotalAt = 0x08;
// One word, spelled bit by bit: LOADOUTS CLASSIC (1), SERIES-10 ONLY (bit 2), PAUSE resuming (2 << 3), the payload map (bit 5),
// driving to the checkpoint (bit 6), PAUSE queued (bit 7), POINT RATE 6 quarters (bits 8-15), PAYLOAD SPEED 8 quarters (bits 16-23).
constexpr std::uint32_t kKnownWord = 0x1u | 0x4u | 0x10u | 0x20u | 0x40u | 0x80u | 0x600u | 0x80000u;
constexpr std::uint8_t kRate6 = 6, kSpeed8 = 8, kLoadoutClassic = 1, kPauseResuming = 2;
constexpr std::uint32_t kCheckpoint2 = 2u << 24;   // the checkpoint used, bits 24-25 (decoded, never packed)
constexpr std::uint32_t kLoadoutThree = 3;         // bits 0-1 = 3: not a LOADOUTS
constexpr std::uint32_t kPauseThree = 3u << 3;     // bits 3-4 = 3: not a PAUSE state

mc::RulesWord KnownRules() {
  mc::RulesWord r;
  r.loadout = kLoadoutClassic;
  r.chassis = true;
  r.paused = rules::Pause::kResuming;
  r.payloadMap = true;
  r.skipping = true;
  r.pauseQueued = true;
  r.pointRate = kRate6;
  r.payloadSpeed = kSpeed8;
  return r;
}

TEST(MatchControlRules, PackLaysTheRulesOutAsTheServersWord) { EXPECT_EQ(mc::Pack(KnownRules()), kKnownWord); }

TEST(MatchControlRules, PackLeavesTheCheckpointOut) {
  mc::RulesWord r = KnownRules();
  r.checkpoint = kPauseResuming;
  EXPECT_EQ(mc::Pack(r), kKnownWord);
}

TEST(MatchControlRules, DecodeReadsEveryField) {
  const mc::RulesWord r = mc::Decode(kKnownWord | kCheckpoint2);
  EXPECT_EQ(r.loadout, kLoadoutClassic);
  EXPECT_TRUE(r.chassis);
  EXPECT_EQ(r.paused, rules::Pause::kResuming);
  EXPECT_TRUE(r.payloadMap);
  EXPECT_TRUE(r.skipping);
  EXPECT_TRUE(r.pauseQueued);
  EXPECT_EQ(r.pointRate, kRate6);
  EXPECT_EQ(r.payloadSpeed, kSpeed8);
  EXPECT_EQ(r.checkpoint, kPauseResuming);
}

TEST(MatchControlRules, ALoadoutsOfThreeReadsAsAllAndAPauseOfThreeAsOff) {
  const mc::RulesWord r = mc::Decode(kLoadoutThree | kPauseThree);
  EXPECT_EQ(r.loadout, mc::kLoadoutAll);
  EXPECT_EQ(r.paused, rules::Pause::kOff);
}

TEST(MatchControlRules, ApplyWritesTheShownBytesAndSaysWhatChanged) {
  std::array<std::uint8_t, kMailboxBytes> m{};
  const mc::Applied a = mc::Apply(m.data(), kKnownWord | kCheckpoint2);
  EXPECT_TRUE(a.rate && a.payload && a.pause && a.loadout && a.chassis);
  EXPECT_EQ(m[kLoadoutsAt], kLoadoutClassic);
  EXPECT_EQ(m[kChassisAt], 1);
  EXPECT_EQ(m[kRateAt], kRate6);
  EXPECT_EQ(m[kPauseAt], kPauseResuming);
  EXPECT_EQ(m[kPayloadMapAt], 1);
  EXPECT_EQ(m[kSpeedAt], kSpeed8);
  EXPECT_EQ(m[kCheckpointAt], kPauseResuming);
  EXPECT_EQ(m[kSkipAt], 1);
  EXPECT_EQ(m[kQueuedAt], 1);
}

TEST(MatchControlRules, TheSameWordAgainChangesNothing) {
  std::array<std::uint8_t, kMailboxBytes> m{};
  mc::Apply(m.data(), kKnownWord);
  EXPECT_FALSE(mc::Apply(m.data(), kKnownWord).Any());
}

TEST(MatchControlRules, ApplyTouchesNoOtherByte) {
  std::array<std::uint8_t, kMailboxBytes> m{};
  m.fill(kFill);
  mc::Apply(m.data(), kKnownWord);
  for (std::uint32_t at = 0; at < kMailboxBytes; ++at) {
    const bool written = at == kLoadoutsAt || at == kChassisAt || at == kRateAt || at == kPauseAt || at == kPayloadMapAt || at == kSpeedAt ||
                         at == kCheckpointAt || at == kSkipAt || at == kQueuedAt;
    if (!written) {
      EXPECT_EQ(m[at], kFill) << "byte +" << at;
    }
  }
}

TEST(MatchControlRules, TheProgressBytesAreWrittenButNotReported) {
  std::array<std::uint8_t, kMailboxBytes> m{};
  mc::Apply(m.data(), kKnownWord);
  m[kSkipAt] = 0;
  m[kQueuedAt] = 0;
  EXPECT_FALSE(mc::Apply(m.data(), kKnownWord).Any());
  EXPECT_EQ(m[kSkipAt], 1);
  EXPECT_EQ(m[kQueuedAt], 1);
}

TEST(MatchControlRules, ClearPutsTheServersBytesBackAndKeepsT) {
  std::array<std::uint8_t, kMailboxBytes> m{};
  m.fill(kFill);
  EXPECT_TRUE(mc::ClearServerRules(m.data()));
  for (std::uint32_t at : {kLoadoutsAt, kChassisAt, kRateAt, kPauseAt, kPayloadMapAt, kSpeedAt, kCheckpointAt, kSkipAt, kQueuedAt})
    EXPECT_EQ(m[at], 0) << "byte +" << at;
  EXPECT_EQ(m[kRoundTotalAt], kFill);
  EXPECT_FALSE(mc::ClearServerRules(m.data()));
}

// T = POINT UNLOCK 2 min, CAPTURE TIME 5 s = 125 s.
constexpr float kTwoMinutesFive = 125.0f;
constexpr int kTwoMinutes = 2, kFiveSeconds = 5;
std::uint32_t BitsOf(float f) {
  std::uint32_t u = 0;
  std::memcpy(&u, &f, sizeof u);
  return u;
}

TEST(MatchControlRules, RoundTotalIsStoredAsItsBitsOnce) {
  std::array<std::uint8_t, kMailboxBytes> m{};
  EXPECT_TRUE(mc::ApplyRoundTotal(m.data(), BitsOf(kTwoMinutesFive)));
  EXPECT_EQ(FloatAt(m, kRoundTotalAt), kTwoMinutesFive);
  EXPECT_FALSE(mc::ApplyRoundTotal(m.data(), BitsOf(kTwoMinutesFive)));
}

TEST(MatchControlRules, PartsSplitTIntoMinutesAndSeconds) {
  const mc::RoundParts t = mc::Parts(BitsOf(kTwoMinutesFive));
  EXPECT_EQ(t.unlockMinutes, kTwoMinutes);
  EXPECT_EQ(t.captureSeconds, kFiveSeconds);
}

TEST(MatchControlRules, ATThatIsNotSecondsReadsAsZero) {
  for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -kTwoMinutesFive,
                    mc::kMaxRoundSeconds * kTwoMinutesFive}) {
    const mc::RoundParts t = mc::Parts(BitsOf(bad));
    EXPECT_EQ(t.unlockMinutes, 0);
    EXPECT_EQ(t.captureSeconds, 0);
  }
}

TEST(MatchControlRules, FourMapsAndNoMore) {
  constexpr std::uint32_t kSurge = 3, kFifth = 4;
  EXPECT_TRUE(mc::MapIndexValid(0));
  EXPECT_TRUE(mc::MapIndexValid(kSurge));
  EXPECT_FALSE(mc::MapIndexValid(kFifth));
  EXPECT_STREQ(mc::kMapNames[kSurge], "SURGE");
}

}  // namespace
