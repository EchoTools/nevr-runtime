// The session game type store (lifecycle/arena_round_time.h): the stored type is read back, and each store returns the
// type it replaces.

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "runtime/lifecycle/arena_round_time.h"

namespace {

TEST(ArenaRoundTime, StoredSessionTypeIsReadBack) {
  nevr_arena_round_time::SetSessionGameType("echo_arena");
  const auto stored = nevr_arena_round_time::SessionGameType();
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(*stored, "echo_arena");

  EXPECT_EQ(nevr_arena_round_time::SetSessionGameType("echo_combat"), std::optional<std::string>("echo_arena"));
  EXPECT_EQ(nevr_arena_round_time::SessionGameType(), std::optional<std::string>("echo_combat"));
}

// #2242 (EchoCombat 2.0.3): VeilNet resets the RULES on each new session, so a store that starts a session must move the
// generation on, and a second store must move it again.
TEST(ArenaRoundTime, EverySessionStoreAdvancesTheGeneration) {
  nevr_arena_round_time::ResetForTest();
  const auto g0 = nevr_arena_round_time::SessionGeneration();
  nevr_arena_round_time::SetSessionGameType("echo_arena_private");
  const auto g1 = nevr_arena_round_time::SessionGeneration();
  nevr_arena_round_time::SetSessionGameType("echo_arena_private");
  EXPECT_GT(g1, g0);
  EXPECT_GT(nevr_arena_round_time::SessionGeneration(), g1);
}

}  // namespace
