#include <gtest/gtest.h>

#include "runtime/log/game_line_format.h"

TEST(GameLineFormat, NullArgumentsLeaveFormatTokensLiteral) {
  char out[64] = {};
  const int len = nevr_game_line_format::Capture(out, sizeof(out), "player %s score %d", nullptr);
  EXPECT_STREQ(out, "player %s score %d");
  EXPECT_EQ(len, 18);
}

TEST(GameLineFormat, NullArgumentsStillRespectTheOutputBound) {
  char out[8] = {};
  const int len = nevr_game_line_format::Capture(out, sizeof(out), "value %s", nullptr);
  EXPECT_STREQ(out, "value %");
  EXPECT_EQ(len, 7);
}
