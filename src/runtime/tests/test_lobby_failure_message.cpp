// test_lobby_failure_message.cpp (#1443): which lobby failure codes take the service's message, and the error block it is
// written into (patch/lobby_failure_message_rules.h). Pure: no game memory, a byte array stands in for the CR15NetGame.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "runtime/patch/lobby_failure_message_rules.h"

namespace lm = nevr_lobby_failure_message;

TEST(LobbyFailureMessage, LiteralCodesTakeTheServiceMessage) {
  for (std::uint8_t code = 0; code <= 10; ++code) EXPECT_TRUE(lm::TakesServiceMessage(code)) << int(code);
  EXPECT_TRUE(lm::TakesServiceMessage(13));
}

TEST(LobbyFailureMessage, CodesThatAlreadyShowTheMessageAreLeftAlone) {
  EXPECT_FALSE(lm::TakesServiceMessage(11));  // banned: the game writes the message itself
  EXPECT_FALSE(lm::TakesServiceMessage(12));  // kicked: the game writes the message itself
  EXPECT_FALSE(lm::TakesServiceMessage(14));
  EXPECT_FALSE(lm::TakesServiceMessage(255));
}

TEST(LobbyFailureMessage, TitleIsTheGamesOwnName) {
  EXPECT_STREQ(lm::Title(8), "lobby full");
  EXPECT_STREQ(lm::Title(9), "internal error");
  EXPECT_STREQ(lm::Title(13), "access denied");
  EXPECT_STREQ(lm::Title(200), "unknown");
}

TEST(LobbyFailureMessage, EmptyMessageWritesNothing) {
  std::array<std::uint8_t, lm::kErrorBlockOffset + 1 + 4 * 64> game{};
  game.fill(0xAA);
  const char empty[64] = {};
  EXPECT_FALSE(lm::WriteErrorBlock(game.data(), "lobby full", empty, sizeof(empty)));
  EXPECT_EQ(game[lm::kErrorBlockOffset], 0xAA);
}

TEST(LobbyFailureMessage, NullBaseWritesNothing) {
  char msg[64] = "server says no";
  EXPECT_FALSE(lm::WriteErrorBlock(nullptr, "lobby full", msg, sizeof(msg)));
}

TEST(LobbyFailureMessage, WritesTitleThenMessageOnTheFourLineBlock) {
  std::array<std::uint8_t, lm::kErrorBlockOffset + 1 + 4 * 64> game{};
  game.fill(0xAA);
  char msg[64] = "server says no";
  ASSERT_TRUE(lm::WriteErrorBlock(game.data(), "lobby full", msg, sizeof(msg)));
  const std::uint8_t* block = game.data() + lm::kErrorBlockOffset;
  EXPECT_EQ(block[0], 1);  // the four-line form
  EXPECT_STREQ(reinterpret_cast<const char*>(block + 1), "lobby full");
  EXPECT_STREQ(reinterpret_cast<const char*>(block + 1 + 64), "server says no");
  EXPECT_EQ(block[1 + 2 * 64], 0);  // line 3 empty
  EXPECT_EQ(block[1 + 3 * 64], 0);  // line 4 empty
}

TEST(LobbyFailureMessage, LongMessageIsCutToSixtyThreeBytesAndNulTerminated) {
  std::array<std::uint8_t, lm::kErrorBlockOffset + 1 + 4 * 64> game{};
  char msg[64];
  std::memset(msg, 'x', sizeof(msg));  // no NUL in the field at all
  ASSERT_TRUE(lm::WriteErrorBlock(game.data(), "t", msg, sizeof(msg)));
  const std::uint8_t* line2 = game.data() + lm::kErrorBlockOffset + 1 + 64;
  for (int i = 0; i < 63; ++i) EXPECT_EQ(line2[i], 'x') << i;
  EXPECT_EQ(line2[63], 0);
}

TEST(LobbyFailureMessage, MessageBytesCountsBeforeTheNul) {
  char msg[64] = "abc";
  EXPECT_EQ(lm::MessageBytes(msg, sizeof(msg)), 3u);
  char full[64];
  std::memset(full, 'y', sizeof(full));
  EXPECT_EQ(lm::MessageBytes(full, sizeof(full)), 64u);
  EXPECT_EQ(lm::MessageBytes(nullptr, 64), 0u);
}
