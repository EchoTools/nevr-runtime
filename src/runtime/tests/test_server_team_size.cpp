// #1422: a dedicated server's private-match team size (patch/server_team_size.h). The two sites are
// driven on buffers that hold the bytes of the pinned echovr.exe, through an injected writer, as
// test_checked_patch.cpp drives the spectator-stream site; no game image is needed.

#include "runtime/patch/server_team_size.h"

#include <gtest/gtest.h>

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

// The bytes at ServerAssignTeam's cap (echovr.exe 0x1401AB8C2) and team slot (0x1401ABC8A), read
// from the pinned build and written here by hand, not taken from the header under test.
const std::vector<uint8_t> kCapStock = {0xb8, 0x05, 0x00, 0x00, 0x00, 0x48, 0xd1, 0xee, 0x48, 0x83, 0xfe, 0x05};
const std::vector<uint8_t> kSlotStock = {0x66, 0xc1, 0xe0, 0x05, 0xb9, 0xe0, 0x03, 0x00, 0x00,
                                         0x66, 0x33, 0xc6, 0x66, 0x23, 0xc1, 0x66, 0x33, 0xc6};
// heisthecat31's EchoRelay slot rule (d2d1a86): cmp al,5; jb +2; sub al,5; shl eax,5; xor eax,esi;
// and ax,0x3e0; xor eax,esi; nop. A sixth member takes slot 0, a seventh slot 1, and so on.
const std::vector<uint8_t> kSlotWrapped = {0x3c, 0x05, 0x72, 0x02, 0x2c, 0x05, 0xc1, 0xe0, 0x05,
                                           0x31, 0xf0, 0x66, 0x25, 0xe0, 0x03, 0x31, 0xf0, 0x90};

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
    EXPECT_EQ(img.cap, CapFor(s)) << int(s);
    EXPECT_EQ(img.slot, kSlotStock) << int(s);
    ASSERT_EQ(g_writes.size(), 1u) << int(s);
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
    EXPECT_EQ(img.slot, kSlotWrapped) << int(s);
    EXPECT_EQ(img.cap, CapFor(s)) << int(s);
    ASSERT_EQ(g_writes.size(), 2u) << int(s);
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

TEST_F(ServerTeamSize, ACapMismatchLeavesBothSitesAsFoundAndKeepsFive) {
  Image img;
  img.cap = CapFor(7);  // another writer set 7
  const std::vector<uint8_t> capFound = img.cap;
  const Outcome o = Run(img, 6);
  EXPECT_TRUE(g_writes.empty()) << "the slot rule must not go in when the cap cannot follow it";
  EXPECT_EQ(img.cap, capFound);
  EXPECT_EQ(img.slot, kSlotStock);
  EXPECT_EQ(o.cap, Result::Mismatch);
  EXPECT_FALSE(o.slotTried);
  EXPECT_EQ(std::memcmp(o.capBefore, capFound.data(), capFound.size()), 0);
  EXPECT_EQ(o.inEffect, 5u);
}

TEST_F(ServerTeamSize, ACapMismatchBelowFiveWritesNothing) {
  Image img;
  img.cap[0] = 0xcc;
  const Outcome o = Run(img, 3);
  EXPECT_TRUE(g_writes.empty());
  EXPECT_EQ(o.cap, Result::Mismatch);
  EXPECT_EQ(o.inEffect, 5u);
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
  EXPECT_EQ(img.cap, kCapStock);
  EXPECT_EQ(o.inEffect, 5u);
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

TEST_F(ServerTeamSize, ASizeOutsideOneToEightWritesNothing) {
  for (uint32_t s : {0u, 9u, 200u}) {
    Image img;
    g_writes.clear();
    const Outcome o = Run(img, s);
    EXPECT_TRUE(g_writes.empty()) << s;
    EXPECT_EQ(o.inEffect, 5u) << s;
  }
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
                        {" 6", "whole"}, {"99999999999", "above"}};
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
  char line[512];
  nevr_server_team_size::FormatRecord(line, sizeof(line), o, 0x1401AB8C2, 0x1401ABC8A);
  const std::string s(line);
  EXPECT_NE(s.find("[NEVR.PATCH] server team size"), std::string::npos) << s;
  EXPECT_NE(s.find("size=6"), std::string::npos) << s;
  EXPECT_NE(s.find("in_effect=5"), std::string::npos) << s;
  EXPECT_NE(s.find("slot_addr=0x1401abc8a"), std::string::npos) << s;
  EXPECT_NE(s.find("slot_expected=66c1e005b9e00300006633c66623c16633c6"), std::string::npos) << s;
  EXPECT_NE(s.find("slot_actual=66c1e005cce00300006633c66623c16633c6"), std::string::npos) << s;
  EXPECT_NE(s.find("slot=mismatch"), std::string::npos) << s;
  EXPECT_NE(s.find("cap_addr=0x1401ab8c2"), std::string::npos) << s;
  EXPECT_NE(s.find("cap=not_tried"), std::string::npos) << s;
}

TEST_F(ServerTeamSize, TheRecordSaysWhenNothingWasWritten) {
  Image img;
  const Outcome o = Run(img, 5);
  char line[512];
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

}  // namespace
