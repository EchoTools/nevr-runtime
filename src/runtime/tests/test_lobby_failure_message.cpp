// test_lobby_failure_message.cpp (#1443): which lobby failure codes take the service's message, and the error block it is
// written into (patch/lobby_failure_message_rules.h). Pure: no game memory, a byte array stands in for the CR15NetGame.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/patch/lobby_failure_message_rules.h"

namespace lm = nevr_lobby_failure_message;

// The seam for the decline (#2006). NoteDeclined takes a plain function pointer, so the capture is a file-level vector of
// (code, reason) pairs: the PC hook passes its writer there, the tests pass CaptureDecline.
namespace {
struct Declined {
  std::uint8_t code;
  lm::Decline reason;
};
std::vector<Declined> g_declines;
void CaptureDecline(std::uint8_t code, lm::Decline reason) {
  g_declines.push_back(Declined{code, reason});
}
}  // namespace

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

TEST(LobbyFailureMessage, DeclineChecksTheBaseFirstAndNamesWhatWriteErrorBlockDeclinedFor) {
  std::array<std::uint8_t, lm::kErrorBlockOffset + 1 + 4 * 64> game{};
  char msg[64] = "server says no";
  char empty[64] = {};
  EXPECT_EQ(lm::DeclineFor(nullptr, msg, sizeof(msg)), lm::Decline::kNoGameBase);
  EXPECT_EQ(lm::DeclineFor(nullptr, empty, sizeof(empty)), lm::Decline::kNoGameBase);  // the base is checked first
  EXPECT_EQ(lm::DeclineFor(game.data(), empty, sizeof(empty)), lm::Decline::kNoMessage);
  EXPECT_EQ(lm::DeclineFor(game.data(), nullptr, 0), lm::Decline::kNoMessage);
  EXPECT_EQ(lm::DeclineFor(game.data(), msg, sizeof(msg)), lm::Decline::kNone);
}

TEST(LobbyFailureMessage, DeclineIsNoneExactlyWhenWriteErrorBlockWrites) {
  std::array<std::uint8_t, lm::kErrorBlockOffset + 1 + 4 * 64> game{};
  char msg[64] = "server says no";
  char empty[64] = {};
  struct Case {
    std::uint8_t* base;
    const char* field;
  };
  const Case cases[] = {{nullptr, msg}, {game.data(), empty}, {game.data(), msg}, {nullptr, empty}};
  for (const Case& c : cases) {
    const bool wrote = lm::WriteErrorBlock(c.base, "lobby full", c.field, sizeof(msg));
    EXPECT_EQ(wrote, lm::DeclineFor(c.base, c.field, sizeof(msg)) == lm::Decline::kNone);
  }
}

TEST(LobbyFailureMessage, DeclineReasonsHaveTheirTokens) {
  EXPECT_STREQ(lm::DeclineToken(lm::Decline::kNoGameBase), "no_game_base");
  EXPECT_STREQ(lm::DeclineToken(lm::Decline::kNoMessage), "no_message");
  EXPECT_STREQ(lm::DeclineToken(lm::Decline::kNone), "none");
}

TEST(LobbyFailureMessage, TheDeclineLineIsTheSharedTextWithCodeTitleReasonAndConsequence) {
  char line[lm::kDeclineLineBytes];
  lm::FormatDeclineLine(line, 8, lm::Decline::kNoMessage);
  EXPECT_STREQ(line,
               "[NEVR.LOBBY] failure message not shown code=8 title=lobby full reason=no_message: "
               "the game shows its fixed literal for this code instead");
}

TEST(LobbyFailureMessage, TheFirstDeclineIsPassedOnceWithItsCodeAndReason) {
  g_declines.clear();
  lm::DeclineLog log;
  EXPECT_TRUE(lm::NoteDeclined(log, 8, lm::Decline::kNoMessage, &CaptureDecline));
  ASSERT_EQ(g_declines.size(), 1u);
  EXPECT_EQ(g_declines[0].code, 8);
  EXPECT_EQ(g_declines[0].reason, lm::Decline::kNoMessage);
}

TEST(LobbyFailureMessage, RepeatsOfADeclineAreNotPassedAgain) {
  g_declines.clear();
  lm::DeclineLog log;
  EXPECT_TRUE(lm::NoteDeclined(log, 8, lm::Decline::kNoMessage, &CaptureDecline));
  for (int i = 0; i < 100; ++i) EXPECT_FALSE(lm::NoteDeclined(log, 8, lm::Decline::kNoMessage, &CaptureDecline)) << i;
  EXPECT_EQ(g_declines.size(), 1u);
}

TEST(LobbyFailureMessage, EachCodeAndReasonGetsItsOwnDecline) {
  g_declines.clear();
  lm::DeclineLog log;
  EXPECT_TRUE(lm::NoteDeclined(log, 8, lm::Decline::kNoMessage, &CaptureDecline));
  EXPECT_TRUE(lm::NoteDeclined(log, 8, lm::Decline::kNoGameBase, &CaptureDecline));
  EXPECT_TRUE(lm::NoteDeclined(log, 9, lm::Decline::kNoMessage, &CaptureDecline));
  ASSERT_EQ(g_declines.size(), 3u);
  EXPECT_EQ(g_declines[1].code, 8);
  EXPECT_EQ(g_declines[1].reason, lm::Decline::kNoGameBase);
  EXPECT_EQ(g_declines[2].code, 9);
  EXPECT_EQ(g_declines[2].reason, lm::Decline::kNoMessage);
}

TEST(LobbyFailureMessage, NoneAndAMissingSinkDoNotUseUpTheDecline) {
  g_declines.clear();
  lm::DeclineLog log;
  EXPECT_FALSE(lm::NoteDeclined(log, 8, lm::Decline::kNone, &CaptureDecline));
  EXPECT_FALSE(lm::NoteDeclined(log, 8, lm::Decline::kNoMessage, nullptr));
  EXPECT_TRUE(g_declines.empty());
  EXPECT_TRUE(lm::NoteDeclined(log, 8, lm::Decline::kNoMessage, &CaptureDecline));
  EXPECT_EQ(g_declines.size(), 1u);
}
