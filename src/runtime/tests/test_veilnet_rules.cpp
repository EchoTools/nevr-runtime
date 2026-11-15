// #2242 (EchoCombat 2.0.3): a new session starts with the default RULES, and VeilNet's watcher reacts once per new session,
// never on the first one the process sees. The mailbox writes are the header's (runtime/combat/rules_state.h), which the
// game's bytes are read through; these tests drive them on a byte array of the same shape.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "runtime/combat/rules_state.h"

namespace {

namespace rules = nevr_combat::rules;

// The default is x1 (4 quarters, runtime/combat/rules_state.h); the test states it itself, so a change to the header's constant
// fails here instead of matching itself.
constexpr std::uint8_t kPointRateX1 = 4;

float FloatAt(const std::array<std::uint8_t, 0x40>& m, std::uint32_t at) {
  float f = 0.0f;
  std::memcpy(&f, m.data() + at, sizeof f);
  return f;
}

TEST(VeilnetRules, DefaultsWriteTheFloatsAndTheShownBytesAFreshSessionStartsWith) {
  std::array<std::uint8_t, 0x40> m{};
  m.fill(0xAB);
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
  std::array<std::uint8_t, 0x40> m{};
  m.fill(0xAB);
  rules::ApplyDefaults(m.data());
  for (std::uint32_t at : {0x10u, 0x12u, 0x13u, 0x14u, 0x16u, 0x2Fu, 0x30u, 0x32u, 0x33u})
    EXPECT_EQ(m[at], 0xAB) << "byte +" << at;
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

}  // namespace
