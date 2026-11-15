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

// The SERIES-10 recursion guard and the standard-chassis flag (#2242, EchoCombat 2.0.5 and 2.0.8). Revault (echovr.exe):
// the flag reader is 0xCE4460, reached from LoadAndApplyItem (0xD0D170) at the call 0xD0D1F4, whose return is 0xD0D1F9; the
// symbol the call passes is the 8 bytes at 0x141CB3420 (the RIP-relative load's aligned address), read as 0x29e261a1df774dd3.
constexpr std::uint64_t kCatStandardFlag = 0x29e261a1df774dd3ull;
constexpr std::uintptr_t kCatFlagRva = 0xCE4460, kCatCallSiteRet = 0xD0D1F9;
constexpr std::uintptr_t kOtherCaller = kCatCallSiteRet + 1;   // any return address but LoadAndApplyItem's own
constexpr int kNotOurs = 0, kInsideOurs = 1, kInsideOursTwice = 2;   // t_chassis_depth

TEST(VeilnetChassis, TheFlagAndItsCallSiteAreEchoCombatsAndRevaults) {
  EXPECT_EQ(gate::kStandardChassisFlag, kCatStandardFlag);
  EXPECT_EQ(gate::kNetFlagRva, kCatFlagRva);
  EXPECT_EQ(gate::kLoadApplyFlagRetRva, kCatCallSiteRet);
  // the bytes revault shows at 0xCE4460 (the function's first 14)
  constexpr std::array<std::uint8_t, 14> kRevault = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};
  EXPECT_TRUE(std::equal(kRevault.begin(), kRevault.end(), gate::kNetFlagHead));
}

TEST(VeilnetChassis, TheFlagReadsZeroOnlyForLoadAndApplyItemsOwnCall) {
  EXPECT_TRUE(gate::FlagForcedZero(kCatStandardFlag, kCatCallSiteRet, kCatCallSiteRet));
  EXPECT_FALSE(gate::FlagForcedZero(kCatStandardFlag, kOtherCaller, kCatCallSiteRet));   // another caller of the flag
  EXPECT_FALSE(gate::FlagForcedZero(kPortA, kCatCallSiteRet, kCatCallSiteRet));     // another flag
}

TEST(VeilnetChassis, ASwapHappensForThePodiumRuleOrTheServersStandardChassisNeverInsideOurOwnCall) {
  // podium rule on: S10, unless the engine's own standard swap is the only thing in the way (no flag hook)
  EXPECT_TRUE(gate::SwapChassisWanted(true, false, true, kNotOurs));
  EXPECT_TRUE(gate::SwapChassisWanted(true, true, true, kNotOurs));
  EXPECT_FALSE(gate::SwapChassisWanted(true, true, false, kNotOurs));
  EXPECT_TRUE(gate::SwapChassisWanted(true, false, false, kNotOurs));
  // podium rule off: S10 only for the server's standard rule, and only with the flag hook to make it S10
  EXPECT_TRUE(gate::SwapChassisWanted(false, true, true, kNotOurs));
  EXPECT_FALSE(gate::SwapChassisWanted(false, false, true, kNotOurs));
  EXPECT_FALSE(gate::SwapChassisWanted(false, true, false, kNotOurs));
  // our own calls (depth above zero) are never swapped again: that is the recursion guard
  EXPECT_FALSE(gate::SwapChassisWanted(true, false, true, kInsideOurs));
  EXPECT_FALSE(gate::SwapChassisWanted(false, true, true, kInsideOursTwice));
}

// The sent-to table both platforms keep (gate::SentTo): a destination is recent while the game sent to it within kDestQuietMs.
TEST(VeilnetGate, ADestinationIsRecentFromTheGamesLastSendOnly) {
  gate::SentTo<int> t;
  constexpr int kSock = 7;
  const nevr_ec::BasicDest<int> a{kSock, kAddrA, kPortA, 0}, b{kSock, kAddrB, kPortB, 0};
  EXPECT_FALSE(t.SentRecently(a, kSentAt));
  t.Note(kSock, kAddrA, kPortA, kSentAt);
  EXPECT_TRUE(t.SentRecently(a, kSentAt + kCatDestQuietMs - 1));
  EXPECT_FALSE(t.SentRecently(a, kSentAt + kCatDestQuietMs));
  EXPECT_FALSE(t.SentRecently(b, kSentAt));                       // another destination
  t.Note(kSock, kAddrA, kPortA, kLater);                          // the next send stamps it again
  EXPECT_TRUE(t.SentRecently(a, kLater));
  const nevr_ec::BasicDest<int> otherSock{kSock + 1, kAddrA, kPortA, 0};
  EXPECT_FALSE(t.SentRecently(otherSock, kLater));                // the socket is part of the key
  t.Clear();
  EXPECT_FALSE(t.SentRecently(a, kLater));
}

TEST(VeilnetGate, AStampOfZeroIsStillASend) {
  gate::SentTo<int> t;
  const nevr_ec::BasicDest<int> a{1, kAddrA, kPortA, 0};
  t.Note(1, kAddrA, kPortA, 0);
  EXPECT_TRUE(t.SentRecently(a, 1));  // stamped 1, not the free-entry 0
}

TEST(VeilnetGate, ANewDestinationTakesTheOldestEntryWhenTheTableIsFull) {
  gate::SentTo<int> t;
  constexpr int kSock = 3;
  constexpr std::uint16_t kFirstPort = 1000;
  for (int i = 0; i < gate::SentTo<int>::kSlots; ++i)
    t.Note(kSock, kAddrA, static_cast<std::uint16_t>(kFirstPort + i), kSentAt + static_cast<std::uint64_t>(i));
  t.Note(kSock, kAddrB, kPortB, kLater);  // one more than the table holds
  const nevr_ec::BasicDest<int> oldest{kSock, kAddrA, kFirstPort, 0}, next{kSock, kAddrA, static_cast<std::uint16_t>(kFirstPort + 1), 0};
  EXPECT_FALSE(t.SentRecently(oldest, kSentAt));   // the oldest was replaced
  EXPECT_TRUE(t.SentRecently(next, kSentAt + 1));  // the others stay
  EXPECT_TRUE(t.SentRecently(nevr_ec::BasicDest<int>{kSock, kAddrB, kPortB, 0}, kLater));
}

}  // namespace
