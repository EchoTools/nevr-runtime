// #982: the public Arena round default applies only when the stored session game type is public Arena.

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "runtime/lifecycle/arena_round_time.h"

namespace {

using nevr_arena_round_time::DecideRoundTimeDefault;

TEST(ArenaRoundTime, PublicArenaSessionGetsTheDefault) {
  const auto decision = DecideRoundTimeDefault(std::optional<std::string>("echo_arena"));
  EXPECT_TRUE(decision.applied);
  EXPECT_EQ(decision.reason, "applied");
}

TEST(ArenaRoundTime, NonArenaSessionKeepsTheGameValue) {
  const auto decision = DecideRoundTimeDefault(std::optional<std::string>("echo_combat"));
  EXPECT_FALSE(decision.applied);
  EXPECT_EQ(decision.reason, "non_arena");
}

// #1493: a social lobby's session used to read as echo_arena through the lobby fallback and got the public default.
TEST(ArenaRoundTime, SocialSessionKeepsTheGameValue) {
  const auto decision = DecideRoundTimeDefault(std::optional<std::string>("social_2.0"));
  EXPECT_FALSE(decision.applied);
  EXPECT_EQ(decision.reason, "non_arena");
}

TEST(ArenaRoundTime, PrivateArenaSessionKeepsTheGameValue) {
  const auto decision = DecideRoundTimeDefault(std::optional<std::string>("echo_arena_private"));
  EXPECT_FALSE(decision.applied);
  EXPECT_EQ(decision.reason, "non_arena");
}

TEST(ArenaRoundTime, NoStoredSessionTypeKeepsTheGameValue) {
  const auto decision = DecideRoundTimeDefault(std::nullopt);
  EXPECT_FALSE(decision.applied);
  EXPECT_EQ(decision.reason, "no_session_type");
}

TEST(ArenaRoundTime, StoredSessionTypeIsReadBack) {
  nevr_arena_round_time::SetSessionGameType("echo_arena");
  const auto stored = nevr_arena_round_time::SessionGameType();
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(*stored, "echo_arena");
  EXPECT_TRUE(DecideRoundTimeDefault(stored).applied);

  nevr_arena_round_time::SetSessionGameType("echo_combat");
  EXPECT_FALSE(DecideRoundTimeDefault(nevr_arena_round_time::SessionGameType()).applied);
}

}  // namespace
