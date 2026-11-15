// #2242 (EchoCombat 2.0.6): VeilNet sends to the server the game still talks to, and only the hello goes to a server that has
// not answered. The decisions are runtime/combat/veilnet_gate.h; the sockets and the clock are veilnet.cpp's.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "runtime/combat/veilnet_gate.h"

namespace {

namespace gate = nevr_combat::gate;
using gate::Hello;

// a server is its address and port in one key (gate::ServerKey, the one packing veilnet.cpp uses too)
constexpr std::uint16_t kPortA = 6792, kPortB = 6793;
constexpr std::uint32_t kAddrA = 0x0A0B0C0D, kAddrB = 0x0A0B0C0E;
constexpr std::uint64_t kServerA = gate::ServerKey(kAddrA, kPortA), kServerB = gate::ServerKey(kAddrB, kPortB);
constexpr std::uint64_t kSentAt = 1000, kLater = 5000;     // clock readings, in ms
constexpr int kTwice = 2, kSilentAfterGiveUp = 3;
// The values EchoCombat 2.0.6 ships (DEST_QUIET_MS, HELLO_TRIES in his veilnet.cpp): stated here, not read from the header, so a change
// to the header's constant fails below instead of matching itself.
constexpr std::uint64_t kCatDestQuietMs = 3000;
constexpr int kCatHelloTries = 5;
constexpr std::size_t kTacModCount = 7;   // EchoCombat's seven tac mods

TEST(VeilnetGate, TheValuesAreEchoCombatsOwn) {
  EXPECT_EQ(gate::kDestQuietMs, kCatDestQuietMs);
  EXPECT_EQ(gate::kHelloTries, kCatHelloTries);
}

// The packing, stated as its fields: the port in the low 16 bits, the address above them (independent of ServerKey's shift).
constexpr int kPortBits = 16;
constexpr std::uint64_t kLowSixteenBits = 0xFFFF;
TEST(VeilnetGate, TheServerKeyHoldsTheAddressAboveThePortBits) {
  EXPECT_EQ(gate::ServerKey(kAddrA, kPortA) >> kPortBits, kAddrA);
  EXPECT_EQ(gate::ServerKey(kAddrA, kPortA) & kLowSixteenBits, kPortA);
  EXPECT_NE(gate::ServerKey(kAddrA, kPortA), gate::ServerKey(kAddrA, kPortB));
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

// The tac mod pick from the local slot (EchoCombat 2.0.8). The seven symbols are EchoCombat's own, stated here.
// EchoCombat's seven tac mod symbols, stated here as the independent pin of nevr_ec::kAbilities.
constexpr std::array<std::uint64_t, kTacModCount> kCatTacMods = {
    0xe32dc7c9d4804fb5ull, 0xe32dc7cad6864aa4ull, 0xc8c33e48386618b6ull, 0xc8c33e4832761fbcull,
    0x41d2d43513260b1aull, 0x41d2d4381430080cull, 0x41d2d02f1b2a1316ull,
};

TEST(VeilnetGate, TheSevenTacModsAreKnownAndNothingElseIs) {
  EXPECT_EQ(sizeof(nevr_ec::kAbilities) / sizeof(nevr_ec::kAbilities[0]), kTacModCount);
  for (std::uint64_t sym : kCatTacMods) EXPECT_TRUE(nevr_ec::KnownTacMod(sym)) << std::hex << sym;
  EXPECT_FALSE(nevr_ec::KnownTacMod(0));
  EXPECT_FALSE(nevr_ec::KnownTacMod(kPortA));
}

TEST(VeilnetGate, ASlotPickIsTakenOnlyWhenItIsANewKnownTacMod) {
  constexpr std::uint64_t kSmoke = 0xe32dc7c9d4804fb5ull, kPoint = 0xe32dc7cad6864aa4ull;
  EXPECT_TRUE(gate::PickFromSlot(kSmoke, 0));
  EXPECT_FALSE(gate::PickFromSlot(kSmoke, kSmoke));   // the same one again: nEVR re-fired the setter
  EXPECT_TRUE(gate::PickFromSlot(kPoint, kSmoke));
  EXPECT_FALSE(gate::PickFromSlot(kPortA, 0));       // not a tac mod
}

}  // namespace
