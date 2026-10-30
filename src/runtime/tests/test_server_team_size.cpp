// #1422: a dedicated server's match team size (patch/server_team_size.h). The two sites are driven on
// buffers that hold the bytes of the pinned echovr.exe, through an injected writer, as test_checked_patch.cpp
// drives the spectator-stream site; no game image is needed. The slot site's bytes, the game's and ours, are run
// natively (ServerTeamSizeSlotCode), so the rule is checked as the processor executes it.

#include "runtime/patch/server_team_size.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using nevr_checked_patch::Result;
using nevr_server_team_size::Apply;
using nevr_server_team_size::Outcome;
using nevr_server_team_size::ParseMaxTeamSize;
using nevr_server_team_size::Plan;
using nevr_server_team_size::PlanFor;

// The bytes at ServerAssignTeam's cap (echovr.exe 0x1401AB8C2) and team slot (0x1401ABC8A, 26 bytes through the
// two movzx copies), read from the pinned build and written here by hand, not taken from the header under test.
const std::vector<uint8_t> kCapStock = {0xb8, 0x05, 0x00, 0x00, 0x00, 0x48, 0xd1, 0xee, 0x48, 0x83, 0xfe, 0x05};
const std::vector<uint8_t> kSlotStock = {0x66, 0xc1, 0xe0, 0x05, 0xb9, 0xe0, 0x03, 0x00, 0x00,
                                         0x66, 0x33, 0xc6, 0x66, 0x23, 0xc1, 0x66, 0x33, 0xc6,
                                         0x44, 0x0f, 0xb7, 0xc8, 0x41, 0x0f, 0xb7, 0xc1};
// The rule above 5: cmp bl,1; ja +6; cmp al,5; jb +2; sub al,5; shl eax,5; xor eax,esi; and eax,0x3e0;
// xor eax,esi; mov r9d,eax. On team 0 or 1 a sixth member takes slot 0, a seventh slot 1; other teams keep the count.
const std::vector<uint8_t> kSlotWrapped = {0x80, 0xfb, 0x01, 0x77, 0x06, 0x3c, 0x05, 0x72, 0x02,
                                           0x2c, 0x05, 0xc1, 0xe0, 0x05, 0x31, 0xf0, 0x25, 0xe0,
                                           0x03, 0x00, 0x00, 0x31, 0xf0, 0x41, 0x89, 0xc1};

std::vector<uint8_t> CapFor(uint8_t size) {
  std::vector<uint8_t> v = kCapStock;
  v[1] = size;
  v[11] = size;
  return v;
}

struct Image {
  std::vector<uint8_t> cap = kCapStock;
  std::vector<uint8_t> slot = kSlotStock;
};

// The writers, as in test_checked_patch.cpp. The order of the writes is recorded so a test can say
// which site was written first.
std::vector<const void*> g_writes;
bool CopyWriter(void* dest, const void* src, size_t n) {
  g_writes.push_back(dest);
  std::memcpy(dest, src, n);
  return true;
}
bool RefusingWriter(void* dest, const void*, size_t) {
  g_writes.push_back(dest);
  return false;
}
// Writes the slot site, refuses the cap site's first write: the cap fails after the slot applied.
uint8_t* g_refuseAt = nullptr;
bool RefuseOneSiteWriter(void* dest, const void* src, size_t n) {
  g_writes.push_back(dest);
  if (dest == g_refuseAt) return false;
  std::memcpy(dest, src, n);
  return true;
}
// As RefuseOneSiteWriter, but a later write to a site already written says it succeeded and writes nothing.
bool RefuseOneThenDropWriter(void* dest, const void* src, size_t n) {
  for (const void* earlier : g_writes) {
    if (earlier == dest) {
      g_writes.push_back(dest);
      return true;
    }
  }
  return RefuseOneSiteWriter(dest, src, n);
}

// Writes every site, but reports its first write to g_refuseAt as failed, as ProcessMemcpy does when the bytes went
// in and the protection restore then failed.
bool WritesButFailsFirstWriter(void* dest, const void* src, size_t n) {
  bool first = true;
  for (const void* earlier : g_writes) first = first && earlier != dest;
  g_writes.push_back(dest);
  std::memcpy(dest, src, n);
  return !(first && dest == g_refuseAt);
}

// Writes only the first 5 bytes at g_refuseAt (the cap's `mov eax, size`) and reports a failure: a cap write that
// landed in part. Every other site is written whole.
bool WritesPartThenFailsWriter(void* dest, const void* src, size_t n) {
  g_writes.push_back(dest);
  if (dest != g_refuseAt) {
    std::memcpy(dest, src, n);
    return true;
  }
  std::memcpy(dest, src, n < 5 ? n : 5);
  return false;
}

class ServerTeamSize : public ::testing::Test {
 protected:
  void SetUp() override {
    g_writes.clear();
    g_refuseAt = nullptr;
  }
  Outcome Run(Image& img, uint32_t size, nevr_checked_patch::WriteFn write = CopyWriter) {
    return Apply(img.cap.data(), img.slot.data(), size, write);
  }
};

// --- the plan for each size ----------------------------------------------------------------------

TEST(ServerTeamSizePlan, FiveIsTheGamesOwnAndOneToFourIsTheCapOnlyAndSixToEightAddsTheSlot) {
  EXPECT_EQ(PlanFor(5), Plan::KeepGame);
  for (uint32_t s = 1; s <= 4; ++s) EXPECT_EQ(PlanFor(s), Plan::CapOnly) << s;
  for (uint32_t s = 6; s <= 8; ++s) EXPECT_EQ(PlanFor(s), Plan::SlotThenCap) << s;
}

// --- the writes ------------------------------------------------------------------------------------

TEST_F(ServerTeamSize, FiveWritesNothing) {
  Image img;
  const Outcome o = Run(img, 5);
  EXPECT_TRUE(g_writes.empty());
  EXPECT_EQ(img.cap, kCapStock);
  EXPECT_EQ(img.slot, kSlotStock);
  EXPECT_FALSE(o.capTried);
  EXPECT_FALSE(o.slotTried);
  EXPECT_EQ(o.inEffect, 5u);
}

TEST_F(ServerTeamSize, OneToFourWriteTheCapsTwoFivesAndLeaveTheSlotAlone) {
  for (uint8_t s = 1; s <= 4; ++s) {
    Image img;
    g_writes.clear();
    const Outcome o = Run(img, s);
    EXPECT_EQ(img.cap, CapFor(s)) << static_cast<int>(s);
    EXPECT_EQ(img.slot, kSlotStock) << static_cast<int>(s);
    ASSERT_EQ(g_writes.size(), 1u) << static_cast<int>(s);
    EXPECT_EQ(g_writes[0], img.cap.data());
    EXPECT_TRUE(o.capTried);
    EXPECT_EQ(o.cap, Result::Applied);
    EXPECT_FALSE(o.slotTried);
    EXPECT_EQ(o.inEffect, uint32_t{s});
  }
}

TEST_F(ServerTeamSize, SixToEightWriteTheSlotRuleFirstThenTheCap) {
  for (uint8_t s = 6; s <= 8; ++s) {
    Image img;
    g_writes.clear();
    const Outcome o = Run(img, s);
    EXPECT_EQ(img.slot, kSlotWrapped) << static_cast<int>(s);
    EXPECT_EQ(img.cap, CapFor(s)) << static_cast<int>(s);
    ASSERT_EQ(g_writes.size(), 2u) << static_cast<int>(s);
    EXPECT_EQ(g_writes[0], img.slot.data()) << "the slot rule goes in before the cap lets a sixth member in";
    EXPECT_EQ(g_writes[1], img.cap.data());
    EXPECT_EQ(o.slot, Result::Applied);
    EXPECT_EQ(o.cap, Result::Applied);
    EXPECT_FALSE(o.slotRestored);
    EXPECT_EQ(o.inEffect, uint32_t{s});
  }
}

TEST_F(ServerTeamSize, ASecondApplyIsAlreadyAppliedAndWritesNothing) {
  Image img;
  Run(img, 6);
  g_writes.clear();
  const Outcome o = Run(img, 6);
  EXPECT_TRUE(g_writes.empty());
  EXPECT_EQ(o.slot, Result::AlreadyApplied);
  EXPECT_EQ(o.cap, Result::AlreadyApplied);
  EXPECT_EQ(o.inEffect, 6u);
}

// --- a site that does not hold the game's bytes ------------------------------------------------------

TEST_F(ServerTeamSize, ACapMismatchLeavesBothSitesAsFoundAndSaysWhatTheyHold) {
  Image img;
  img.cap = CapFor(7);  // another writer set 7
  const std::vector<uint8_t> capFound = img.cap;
  const Outcome o = Run(img, 6);
  EXPECT_TRUE(g_writes.empty()) << "the slot rule must not go in when the cap cannot follow it";
  EXPECT_EQ(img.cap, capFound);
  EXPECT_EQ(img.slot, kSlotStock);
  EXPECT_TRUE(o.capTried);
  EXPECT_EQ(o.cap, Result::Mismatch);
  EXPECT_FALSE(o.slotTried);
  EXPECT_EQ(std::memcmp(o.capBefore, capFound.data(), capFound.size()), 0);
  EXPECT_EQ(o.inEffect, 7u) << "the size in effect is what the cap site holds, not the game's 5";
  EXPECT_TRUE(nevr_server_team_size::IsFailure(o));
}

TEST_F(ServerTeamSize, ACapMismatchBelowFiveWritesNothing) {
  Image img;
  img.cap[0] = 0xcc;
  const Outcome o = Run(img, 3);
  EXPECT_TRUE(g_writes.empty());
  EXPECT_EQ(o.cap, Result::Mismatch);
  EXPECT_EQ(o.inEffect, 0u) << "bytes that are not the game's shape put no known size in effect";
}

TEST_F(ServerTeamSize, ASlotMismatchLeavesBothSitesAsFoundAndKeepsFive) {
  Image img;
  img.slot[4] = 0xcc;
  const std::vector<uint8_t> slotFound = img.slot;
  const Outcome o = Run(img, 8);
  EXPECT_TRUE(g_writes.empty());
  EXPECT_EQ(img.cap, kCapStock);
  EXPECT_EQ(img.slot, slotFound);
  EXPECT_EQ(o.slot, Result::Mismatch);
  EXPECT_FALSE(o.capTried) << "the cap is not raised past five without the slot rule";
  EXPECT_EQ(std::memcmp(o.slotBefore, slotFound.data(), slotFound.size()), 0);
  EXPECT_EQ(o.inEffect, 5u);
}

TEST_F(ServerTeamSize, ARefusedSlotWriteLeavesTheCapAtFive) {
  Image img;
  const Outcome o = Run(img, 6, RefusingWriter);
  EXPECT_EQ(o.slot, Result::WriteFailed);
  EXPECT_FALSE(o.capTried);
  EXPECT_EQ(img.cap, kCapStock);
  EXPECT_EQ(o.inEffect, 5u);
}

TEST_F(ServerTeamSize, ACapWriteThatFailsAfterTheSlotAppliedPutsTheSlotBack) {
  Image img;
  g_refuseAt = img.cap.data();
  const Outcome o = Run(img, 6, RefuseOneSiteWriter);
  EXPECT_EQ(o.slot, Result::Applied);
  EXPECT_EQ(o.cap, Result::WriteFailed);
  EXPECT_TRUE(o.slotRestored);
  EXPECT_EQ(img.slot, kSlotStock) << "a server left with the slot rule and a cap of five is not the game's own";
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  EXPECT_NE(std::string(line).find("the slot rule was put back"), std::string::npos) << line;
  EXPECT_NE(std::string(line).find("slot_restored=true"), std::string::npos) << line;
  EXPECT_EQ(img.cap, kCapStock);
  EXPECT_EQ(o.inEffect, 5u);
}

TEST_F(ServerTeamSize, ARestoreThatDoesNotReadBackIsNotCalledRestored) {
  Image img;
  g_refuseAt = img.cap.data();
  const Outcome o = Run(img, 6, RefuseOneThenDropWriter);
  EXPECT_EQ(o.cap, Result::WriteFailed);
  EXPECT_FALSE(o.slotRestored);
  EXPECT_EQ(img.slot, kSlotWrapped);
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  EXPECT_EQ(std::string(line).find("slot_restored"), std::string::npos) << line;
}

TEST_F(ServerTeamSize, ASlotThatWasAlreadyWrappedIsNotPutBackWhenTheCapFails) {
  Image img;
  img.slot = kSlotWrapped;  // another writer put the slot rule in; it is not ours to take out
  g_refuseAt = img.cap.data();
  const Outcome o = Run(img, 6, RefuseOneSiteWriter);
  EXPECT_EQ(o.slot, Result::AlreadyApplied);
  EXPECT_EQ(o.cap, Result::WriteFailed);
  EXPECT_FALSE(o.slotRestored);
  EXPECT_EQ(img.slot, kSlotWrapped);
}

TEST_F(ServerTeamSize, ACapWriteThatReportsAFailureAfterWritingKeepsTheSlotRule) {
  Image img;
  g_refuseAt = img.cap.data();
  const Outcome o = Run(img, 6, WritesButFailsFirstWriter);
  EXPECT_EQ(o.slot, Result::Applied);
  EXPECT_EQ(o.cap, Result::WriteFailed);
  EXPECT_EQ(img.cap, CapFor(6));
  EXPECT_FALSE(o.slotRestored);
  EXPECT_EQ(img.slot, kSlotWrapped) << "a cap of six with the game's slot rule gives a sixth member slot 5";
  EXPECT_EQ(o.inEffect, 6u);
  EXPECT_TRUE(nevr_server_team_size::IsFailure(o)) << "the write's error is the operator's to hear";
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  EXPECT_NE(std::string(line).find("in_effect=6 (in effect, but a write reported an error)"), std::string::npos)
      << line;
}

TEST_F(ServerTeamSize, ACapWrittenInPartKeepsTheSlotRule) {
  Image img;
  g_refuseAt = img.cap.data();
  const Outcome o = Run(img, 6, WritesPartThenFailsWriter);
  EXPECT_EQ(o.slot, Result::Applied);
  EXPECT_EQ(o.cap, Result::WriteFailed);
  EXPECT_EQ(img.cap[1], 6) << "the `mov eax, 6` went in";
  EXPECT_EQ(img.cap[11], 5) << "the `cmp rsi, 5` did not";
  EXPECT_EQ(o.inEffect, 0u) << "bytes no cap reads";
  EXPECT_FALSE(o.slotRestored) << "the cmova may still give a cap of 6, which needs the slot rule";
  EXPECT_EQ(img.slot, kSlotWrapped);
  EXPECT_TRUE(o.slotRule);
  EXPECT_TRUE(nevr_server_team_size::IsFailure(o));
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  EXPECT_NE(std::string(line).find(
                "in_effect=unknown (the cap site holds bytes that are no cap this reads, so the slot rule stays in)"),
            std::string::npos)
      << line;
}

TEST_F(ServerTeamSize, ACapWrittenInPartBelowFiveIsNotInEffect) {
  Image img;
  g_refuseAt = img.cap.data();
  const Outcome o = Run(img, 4, WritesPartThenFailsWriter);
  EXPECT_EQ(o.cap, Result::WriteFailed);
  EXPECT_EQ(o.inEffect, 0u);
  EXPECT_FALSE(o.slotRule) << "1 to 4 never write the slot site";
  EXPECT_EQ(img.slot, kSlotStock);
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  EXPECT_NE(std::string(line).find("in_effect=unknown (not in effect)"), std::string::npos) << line;
}

TEST_F(ServerTeamSize, ASlotWriteThatReportsAFailureAfterWritingIsPutBack) {
  Image img;
  g_refuseAt = img.slot.data();
  const Outcome o = Run(img, 6, WritesButFailsFirstWriter);
  EXPECT_EQ(o.slot, Result::WriteFailed);
  EXPECT_FALSE(o.capTried);
  EXPECT_TRUE(o.slotRestored);
  EXPECT_EQ(img.slot, kSlotStock) << "the cap stayed five, so the slot rule goes back out";
  EXPECT_EQ(img.cap, kCapStock);
  EXPECT_EQ(o.inEffect, 5u);
}

TEST_F(ServerTeamSize, AnotherWritersSixWithARefusedSlotWriteIsAFailure) {
  Image img;
  img.cap = CapFor(6);
  const Outcome o = Run(img, 6, RefusingWriter);
  EXPECT_EQ(o.slot, Result::WriteFailed);
  EXPECT_EQ(o.inEffect, 6u);
  EXPECT_FALSE(o.slotRule);
  EXPECT_TRUE(o.slotGame);
  EXPECT_TRUE(nevr_server_team_size::IsFailure(o));
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  EXPECT_NE(std::string(line).find("in_effect=6 (the cap is in without the slot rule, so a sixth member takes slot 5)"),
            std::string::npos)
      << line;
}

TEST_F(ServerTeamSize, AnotherWritersCapOfTheSizeAskedGetsTheSlotRuleAndKeepsIt) {
  Image img;
  img.cap = CapFor(6);  // a writer before us put only the cap in
  const Outcome o = Run(img, 6);
  EXPECT_EQ(o.slot, Result::Applied);
  EXPECT_EQ(o.cap, Result::AlreadyApplied);
  EXPECT_FALSE(o.slotRestored) << "the cap is above 5, so the slot rule stays";
  EXPECT_EQ(img.slot, kSlotWrapped);
  EXPECT_EQ(img.cap, CapFor(6));
  EXPECT_EQ(o.inEffect, 6u);
  EXPECT_TRUE(o.slotRule);
  EXPECT_FALSE(nevr_server_team_size::IsFailure(o));
}

TEST_F(ServerTeamSize, AnotherWritersSixWithoutTheSlotRuleIsAFailure) {
  Image img;
  img.cap = CapFor(6);
  img.slot[4] = 0xcc;  // neither the game's slot bytes nor the rule
  const Outcome o = Run(img, 6);
  EXPECT_EQ(o.slot, Result::Mismatch);
  EXPECT_EQ(o.inEffect, 6u);
  EXPECT_FALSE(o.slotRule);
  EXPECT_FALSE(o.slotGame);
  EXPECT_TRUE(nevr_server_team_size::IsFailure(o)) << "six is in at the cap; where a sixth member goes is not known";
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  EXPECT_NE(std::string(line).find("in_effect=6 (the cap is in without our slot rule, and the slot site holds another "
                                   "writer's bytes)"),
            std::string::npos)
      << line;
}

TEST_F(ServerTeamSize, BelowSixTheSlotRuleIsNotNeeded) {
  Image img;
  const Outcome o = Run(img, 4);
  EXPECT_FALSE(o.slotRule);
  EXPECT_FALSE(nevr_server_team_size::IsFailure(o));
  Image wrapped;
  wrapped.slot = kSlotWrapped;  // another writer's rule, harmless below 6
  EXPECT_FALSE(nevr_server_team_size::IsFailure(Run(wrapped, 4)));
}

TEST_F(ServerTeamSize, ASizeOutsideOneToEightWritesNothing) {
  for (uint32_t s : {0u, 9u, 200u}) {
    Image img;
    g_writes.clear();
    const Outcome o = Run(img, s);
    EXPECT_TRUE(g_writes.empty()) << s;
    EXPECT_EQ(o.inEffect, 5u) << s;
    EXPECT_FALSE(nevr_server_team_size::IsFailure(o)) << s << ": nothing was asked that the game does not hold";
  }
}

TEST_F(ServerTeamSize, NothingAskedButAnotherWritersCapIsAFailure) {
  Image img;
  img.cap = CapFor(7);
  const Outcome o = Run(img, 5);
  EXPECT_TRUE(g_writes.empty());
  EXPECT_EQ(o.inEffect, 7u);
  EXPECT_TRUE(nevr_server_team_size::IsFailure(o));
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  EXPECT_NE(std::string(line).find("in_effect=7 (nothing written; the cap site holds another writer's bytes)"),
            std::string::npos)
      << line;
  // Not only a cap above 5 without the slot rule: any cap but the game's five, when nothing was asked.
  Image three;
  three.cap = CapFor(3);
  EXPECT_TRUE(nevr_server_team_size::IsFailure(Run(three, 5)));
  Image sevenWithTheRule;
  sevenWithTheRule.cap = CapFor(7);
  sevenWithTheRule.slot = kSlotWrapped;
  EXPECT_TRUE(nevr_server_team_size::IsFailure(Run(sevenWithTheRule, 5)));
  Image unknown;
  unknown.cap[0] ^= 0xFF;
  const Outcome u = Run(unknown, 5);
  EXPECT_EQ(u.inEffect, 0u);
  EXPECT_TRUE(nevr_server_team_size::IsFailure(u));
}

TEST(ServerTeamSizeCap,TheSizeInEffectIsReadFromTheBytes) {
  for (uint8_t s = 1; s <= 8; ++s) EXPECT_EQ(nevr_server_team_size::CapInEffect(CapFor(s).data()), uint32_t{s});
  std::vector<uint8_t> uneven = CapFor(6);
  uneven[1] = 7;  // the two immediates disagree
  EXPECT_EQ(nevr_server_team_size::CapInEffect(uneven.data()), 0u);
  std::vector<uint8_t> other = kCapStock;
  other[6] = 0xcc;
  EXPECT_EQ(nevr_server_team_size::CapInEffect(other.data()), 0u);
}

// --- the config value ---------------------------------------------------------------------------

TEST(ServerTeamSizeParse, AbsentOrEmptyIsTheGamesFive) {
  std::string problem = "stale";
  EXPECT_EQ(ParseMaxTeamSize(nullptr, &problem), 5u);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ParseMaxTeamSize("", &problem), 5u);
  EXPECT_TRUE(problem.empty());
}

TEST(ServerTeamSizeParse, OneToEightAreTaken) {
  const char* texts[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
  for (uint32_t s = 1; s <= 8; ++s) {
    std::string problem;
    EXPECT_EQ(ParseMaxTeamSize(texts[s - 1], &problem), s);
    EXPECT_TRUE(problem.empty()) << s;
  }
}

TEST(ServerTeamSizeParse, AnythingElseIsFiveWithTheReason) {
  struct Case {
    const char* text;
    const char* reason;
  };
  const Case cases[] = {{"0", "below"}, {"9", "above"},       {"16", "above"},
                        {"-1", "whole"}, {"x", "whole"},       {"5.5", "whole"},
                        {" 6", "whole"}, {"99999999999", "above"}, {":", "whole"},
                        {"/", "whole"}, {"4294967302", "above"}};  // 2^32 + 6: read past the limit, not wrapped to 6
  for (const Case& c : cases) {
    std::string problem;
    EXPECT_EQ(ParseMaxTeamSize(c.text, &problem), 5u) << c.text;
    EXPECT_NE(problem.find(c.reason), std::string::npos) << c.text << ": " << problem;
  }
}

// --- the log record -----------------------------------------------------------------------------

TEST_F(ServerTeamSize, TheRecordGivesEachSitesAddressExpectedAndFoundBytesAndResult) {
  Image img;
  img.slot[4] = 0xcc;
  const Outcome o = Run(img, 6);
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  const std::string s(line);
  EXPECT_NE(s.find("[NEVR.PATCH] server team size"), std::string::npos) << s;
  EXPECT_NE(s.find("size=6"), std::string::npos) << s;
  EXPECT_NE(s.find("in_effect=5 (not in effect)"), std::string::npos) << s;
  EXPECT_NE(s.find("slot_addr=0x1401abc8a"), std::string::npos) << s;
  EXPECT_NE(s.find("slot_expected=66c1e005b9e00300006633c66623c16633c6440fb7c8410fb7c1"), std::string::npos) << s;
  EXPECT_NE(s.find("slot_actual=66c1e005cce00300006633c66623c16633c6440fb7c8410fb7c1"), std::string::npos) << s;
  EXPECT_NE(s.find("slot=mismatch"), std::string::npos) << s;
  EXPECT_NE(s.find("cap_addr=0x1401ab8c2"), std::string::npos) << s;
  EXPECT_NE(s.find("cap=not_tried"), std::string::npos) << s;
}

TEST_F(ServerTeamSize, TheRecordGivesTheCapsExpectedAndFoundBytes) {
  Image img;
  img.cap[6] = 0xcc;
  const Outcome o = Run(img, 3);
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  const std::string s(line);
  EXPECT_NE(s.find("in_effect=unknown"), std::string::npos) << s;
  EXPECT_NE(s.find("cap_expected=b80500000048d1ee4883fe05"), std::string::npos) << s;
  EXPECT_NE(s.find("cap_actual=b80500000048ccee4883fe05"), std::string::npos) << s;
  EXPECT_NE(s.find("cap=mismatch"), std::string::npos) << s;
}

TEST_F(ServerTeamSize, ASuccessSaysWhatIsInEffectWithoutBytes) {
  for (uint8_t size : {3, 6}) {
    Image img;
    const Outcome o = Run(img, size);
    char line[640];
    nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
    const std::string s(line);
    const std::string head = "[NEVR.PATCH] server team size size=" + std::to_string(size) +
                             " in_effect=" + std::to_string(size) + " (a match team holds up to the size";
    EXPECT_EQ(s.rfind(head, 0), 0u) << s;
    EXPECT_EQ(s.find("_expected="), std::string::npos) << "no bytes at Info: " << s;
    EXPECT_EQ(s.find("_addr="), std::string::npos) << s;
    EXPECT_NE(s.find(size > 5 ? "cap=applied slot=applied" : "cap=applied slot=not_tried"), std::string::npos) << s;
    EXPECT_EQ(s.find("more than 10 players") != std::string::npos, size > 5) << s;
  }
}

TEST_F(ServerTeamSize, TheRecordSaysWhenNothingWasWritten) {
  Image img;
  const Outcome o = Run(img, 5);
  char line[640];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  const std::string s(line);
  EXPECT_NE(s.find("size=5 in_effect=5"), std::string::npos) << s;
  EXPECT_NE(s.find("nothing written"), std::string::npos) << s;
  EXPECT_FALSE(nevr_server_team_size::IsFailure(o));
}

TEST_F(ServerTeamSize, AFailureIsOnlyASizeThatWasAskedForAndNotPutInEffect) {
  Image ok;
  EXPECT_FALSE(nevr_server_team_size::IsFailure(Run(ok, 6)));
  Image bad;
  bad.cap[0] = 0xcc;
  EXPECT_TRUE(nevr_server_team_size::IsFailure(Run(bad, 6)));
}

// --- what Install logs -------------------------------------------------------------------------------

// The error the writer reports for a failed write, as LastWriteError does in server_team_size.cpp.
uint32_t g_writeError = 0;
uint32_t TestWriteError() { return g_writeError; }

std::vector<nevr_server_team_size::Line> Install(bool isServer, const char* text, Image& img,
                                                 nevr_checked_patch::WriteFn write = CopyWriter) {
  return nevr_server_team_size::InstallLines(isServer, text, img.cap.data(), img.slot.data(), write, TestWriteError,
                                             0x1401AB8C2, 0x1401ABC8A);
}

TEST_F(ServerTeamSize, AClientLogsOneInfoSkipLineAndTouchesNothing) {
  Image img;
  const auto lines = Install(false, "6", img);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_FALSE(lines[0].warning);
  EXPECT_EQ(lines[0].text, "[NEVR.PATCH] server team size not applied: this is a client (the server assigns teams)");
  EXPECT_TRUE(g_writes.empty());
  EXPECT_EQ(img.cap, kCapStock);
}

TEST_F(ServerTeamSize, AServerLogsTheRecordWithItsSource) {
  Image fromConfig;
  auto lines = Install(true, "6", fromConfig);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_FALSE(lines[0].warning);
  EXPECT_EQ(lines[0].text.rfind("[NEVR.PATCH] server team size size=6 in_effect=6 ", 0), 0u) << lines[0].text;
  EXPECT_NE(lines[0].text.find(" source=config.yaml"), std::string::npos) << lines[0].text;
  EXPECT_EQ(lines[0].text.find("write_error="), std::string::npos) << "no write failed: " << lines[0].text;
  EXPECT_EQ(fromConfig.cap, CapFor(6));
  Image absent;
  lines = Install(true, nullptr, absent);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].text.find("size=5 in_effect=5 (the game's own cap, nothing written)"), std::string::npos);
  EXPECT_NE(lines[0].text.find(" source=default"), std::string::npos) << lines[0].text;
  Image empty;
  lines = Install(true, "", empty);
  EXPECT_NE(lines.back().text.find(" source=default"), std::string::npos) << lines.back().text;
}

TEST_F(ServerTeamSize, ABadValueIsAWarningThenTheGamesFive) {
  Image img;
  const auto lines = Install(true, "9", img);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_TRUE(lines[0].warning);
  EXPECT_EQ(lines[0].text,
            "[NEVR.PATCH] network.max_team_size=9 is above the limit of 8 (allowed 1..8); using the game's 5");
  EXPECT_FALSE(lines[1].warning);
  EXPECT_NE(lines[1].text.find("size=5 in_effect=5"), std::string::npos) << lines[1].text;
  EXPECT_NE(lines[1].text.find(" source=config.yaml"), std::string::npos) << lines[1].text;
  EXPECT_TRUE(g_writes.empty());
}

TEST_F(ServerTeamSize, ASizeNotPutInEffectIsAWarningWithEachSitesBytes) {
  Image img;
  img.slot[4] = 0xcc;
  const auto lines = Install(true, "7", img);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(lines[0].warning);
  EXPECT_NE(lines[0].text.find("cap_addr=0x1401ab8c2"), std::string::npos) << lines[0].text;
  EXPECT_NE(lines[0].text.find("slot_actual="), std::string::npos) << lines[0].text;
}

TEST_F(ServerTeamSize, AFailedWriteCarriesTheWritersError) {
  g_writeError = 5;  // ERROR_ACCESS_DENIED
  Image slotRefused;
  g_refuseAt = slotRefused.slot.data();
  auto lines = Install(true, "6", slotRefused, RefuseOneSiteWriter);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(lines[0].warning);
  EXPECT_NE(lines[0].text.find(" slot=write_failed write_error=5 source=config.yaml"), std::string::npos)
      << lines[0].text;
  g_writeError = 487;  // ERROR_INVALID_ADDRESS
  Image capRefused;
  g_refuseAt = capRefused.cap.data();
  lines = Install(true, "6", capRefused, RefuseOneSiteWriter);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].text.find("cap=write_failed"), std::string::npos) << lines[0].text;
  EXPECT_NE(lines[0].text.find(" write_error=487 source=config.yaml"), std::string::npos) << lines[0].text;
  g_writeError = 0;
}

TEST(ServerTeamSizeWriteErrors, TheLastFailedWritesErrorIsKept) {
  nevr_server_team_size::WriteErrors e;
  EXPECT_TRUE(e.Note(true, 7));
  EXPECT_EQ(e.last, 0u) << "a write that succeeded leaves no error";
  EXPECT_FALSE(e.Note(false, 5));
  EXPECT_EQ(e.last, 5u);
  EXPECT_TRUE(e.Note(true, 9));
  EXPECT_EQ(e.last, 5u) << "a later success keeps the failure";
  EXPECT_FALSE(e.Note(false, 6));
  EXPECT_EQ(e.last, 6u) << "the last failure wins";
}

// --- the slot site's code, run --------------------------------------------------------------------------

// Runs a slot sequence natively: count in eax (zero-extended), the member's word in esi, the team in ebx, as
// ServerAssignTeam holds them at 0x1401ABC8A. Returns eax in the low half and r9d in the high half.
class SlotCode {
 public:
  explicit SlotCode(const std::vector<uint8_t>& body) {
    // push rbx; push rsi; mov eax, ecx; mov esi, edx; mov ebx, r8d; <body>; shl r9, 32; mov eax, eax;
    // or rax, r9; pop rsi; pop rbx; ret
    std::vector<uint8_t> code = {0x53, 0x56, 0x89, 0xc8, 0x89, 0xd6, 0x44, 0x89, 0xc3};
    code.insert(code.end(), body.begin(), body.end());
    const uint8_t tail[] = {0x49, 0xc1, 0xe1, 0x20, 0x89, 0xc0, 0x4c, 0x09, 0xc8, 0x5e, 0x5b, 0xc3};
    code.insert(code.end(), std::begin(tail), std::end(tail));
    mem_ = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (mem_ != nullptr) std::memcpy(mem_, code.data(), code.size());
  }
  ~SlotCode() {
    if (mem_ != nullptr) VirtualFree(mem_, 0, MEM_RELEASE);
  }
  SlotCode(const SlotCode&) = delete;
  SlotCode& operator=(const SlotCode&) = delete;
  bool ok() const { return mem_ != nullptr; }
  uint64_t Run(uint32_t count, uint32_t word, uint32_t team) const {
    using Fn = uint64_t (*)(uint32_t, uint32_t, uint32_t);
    return reinterpret_cast<Fn>(mem_)(count, word, team);
  }

 private:
  void* mem_ = nullptr;
};

uint32_t WithSlot(uint32_t word, uint32_t slot) { return (word & ~0x3e0u) | ((slot & 0x1f) << 5); }

TEST(ServerTeamSizeSlotCode, TheGamesCodeStoresTheCountAsTheSlot) {
  SlotCode code(kSlotStock);
  ASSERT_TRUE(code.ok());
  for (uint32_t team : {0u, 1u, 3u}) {
    for (uint32_t count = 0; count <= 12; ++count) {
      for (uint32_t word : {0x0000u, 0xffffu, 0x1c1fu}) {
        const uint64_t r = code.Run(count, word, team);
        EXPECT_EQ(static_cast<uint32_t>(r), WithSlot(word, count)) << team << " " << count << " " << word;
        EXPECT_EQ(static_cast<uint32_t>(r >> 32), WithSlot(word, count)) << "r9d is the same word";
      }
    }
  }
}

TEST(ServerTeamSizeSlotCode, OurRuleWrapsTheTwoMatchTeamsOnly) {
  ASSERT_EQ(std::vector<uint8_t>(nevr_server_team_size::kSlotPatched, nevr_server_team_size::kSlotPatched + 26),
            kSlotWrapped);
  SlotCode code(kSlotWrapped);
  ASSERT_TRUE(code.ok());
  for (uint32_t team = 0; team <= 7; ++team) {
    for (uint32_t count = 0; count <= 12; ++count) {
      const uint32_t slot = team <= 1 && count >= 5 ? count - 5 : count;
      for (uint32_t word : {0x0000u, 0xffffu, 0x1c1fu}) {
        const uint64_t r = code.Run(count, word, team);
        EXPECT_EQ(static_cast<uint32_t>(r), WithSlot(word, slot)) << team << " " << count << " " << word;
        EXPECT_EQ(static_cast<uint32_t>(r >> 32), WithSlot(word, slot)) << "r9d is the same word";
      }
    }
  }
}

}  // namespace
