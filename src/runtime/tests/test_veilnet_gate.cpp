// #2242 (EchoCombat 2.0.6): VeilNet sends to the server the game still talks to, and only the hello goes to a server that has
// not answered. The decisions are runtime/combat/veilnet_gate.h; the sockets and the clock are veilnet.cpp's.
#include <gtest/gtest.h>

#include <cstdint>

#include "runtime/combat/veilnet_gate.h"

namespace {

namespace gate = nevr_combat::gate;
using gate::Hello;

// a server is its address and port in one key (the veilnet.cpp side packs them the same way)
constexpr std::uint64_t kPortA = 6792, kPortB = 6793, kAddrA = 0x0A0B0C0Dull, kAddrB = 0x0A0B0C0Eull;
constexpr std::uint64_t kServerA = kAddrA << 16 | kPortA, kServerB = kAddrB << 16 | kPortB;
constexpr std::uint64_t kSentAt = 1000, kLater = 5000;     // clock readings, in ms
constexpr int kTwice = 2, kSilentAfterGiveUp = 3;
// The values EchoCombat 2.0.6 ships (DEST_QUIET_MS, HELLO_TRIES in his veilnet.cpp): stated here, not read from the header, so a change
// to the header's constant fails below instead of matching itself.
constexpr std::uint64_t kCatDestQuietMs = 3000;
constexpr int kCatHelloTries = 5;

TEST(VeilnetGate, TheValuesAreEchoCombatsOwn) {
  EXPECT_EQ(gate::kDestQuietMs, kCatDestQuietMs);
  EXPECT_EQ(gate::kHelloTries, kCatHelloTries);
}

TEST(VeilnetGate, ADestinationIsRecentOnlyWhileTheGameSendsWithinTheWindow) {
  EXPECT_FALSE(gate::Recent(0, kLater, gate::kDestQuietMs));                   // never sent to
  EXPECT_TRUE(gate::Recent(kSentAt, kSentAt, gate::kDestQuietMs));             // just sent
  EXPECT_TRUE(gate::Recent(kSentAt, kSentAt + gate::kDestQuietMs - 1, gate::kDestQuietMs));
  EXPECT_FALSE(gate::Recent(kSentAt, kSentAt + gate::kDestQuietMs, gate::kDestQuietMs));  // the window is over
}

TEST(VeilnetGate, OnlyTheHelloGoesToAServerThatHasNotAnswered) {
  EXPECT_TRUE(gate::SendAllowed('H', false));
  EXPECT_TRUE(gate::SendAllowed('H', true));
  for (std::uint8_t type : {'V', 'P', 'T', 'O', 'C', 'M', 'L', 'R', 'K'}) {
    EXPECT_FALSE(gate::SendAllowed(type, false)) << static_cast<char>(type);
    EXPECT_TRUE(gate::SendAllowed(type, true)) << static_cast<char>(type);
  }
}

TEST(VeilnetGate, AServerThatNeverAnswersGetsFiveHellosThenNoMore) {
  gate::HelloTries t;
  for (int i = 1; i < gate::kHelloTries; ++i) EXPECT_EQ(t.Next(kServerA, false), Hello::kSend) << "hello " << i;
  EXPECT_EQ(t.Next(kServerA, false), Hello::kSendAndGiveUp) << "the last one says so";
  for (int i = 0; i < kSilentAfterGiveUp; ++i) EXPECT_EQ(t.Next(kServerA, false), Hello::kNone);
}

TEST(VeilnetGate, ANoServerStartsTheCountAgainOnTheNextOne) {
  gate::HelloTries t;
  for (int i = 0; i < gate::kHelloTries; ++i) t.Next(kServerA, false);
  EXPECT_EQ(t.Next(kServerA, false), Hello::kNone);
  EXPECT_EQ(t.Next(0, false), Hello::kNone);                  // the game is not connected: nothing to say hello to
  EXPECT_EQ(t.Next(kServerA, false), Hello::kSend);           // the same server again, after the game came back
}

TEST(VeilnetGate, AnotherServerStartsTheCountAgain) {
  gate::HelloTries t;
  for (int i = 0; i < gate::kHelloTries; ++i) t.Next(kServerA, false);
  EXPECT_EQ(t.Next(kServerA, false), Hello::kNone);
  EXPECT_EQ(t.Next(kServerB, false), Hello::kSend);
}

TEST(VeilnetGate, AnAnsweringServerIsHelloedEveryTime) {
  gate::HelloTries t;
  for (int i = 0; i < kTwice * gate::kHelloTries; ++i) EXPECT_EQ(t.Next(kServerA, true), Hello::kSend) << i;
  // it goes silent: the hello that went out with its last answer is the first of the count, so the count runs from there
  for (int i = 2; i < gate::kHelloTries; ++i) EXPECT_EQ(t.Next(kServerA, false), Hello::kSend) << "hello " << i;
  EXPECT_EQ(t.Next(kServerA, false), Hello::kSendAndGiveUp);
}

}  // namespace
