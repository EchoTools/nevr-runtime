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

}  // namespace
