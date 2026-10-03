#include <gtest/gtest.h>

#include <set>
#include <string>

#include "runtime/lifecycle/readable_memory.h"
#include "runtime/lifecycle/crash_recovery_sites.h"
#include "runtime/lifecycle/crash_dump_format.h"
#include "runtime/lifecycle/stack_alloc_check.h"

TEST(CrashRecoveryN71Sites, TableIsCompleteAndWellFormed) {
  EXPECT_EQ(CrashRecovery::kKnownNullDerefSites.size(), 30U);
  std::set<uint64_t> rvas;
  for (const CrashRecovery::KnownNullDerefSite& site : CrashRecovery::kKnownNullDerefSites) {
    EXPECT_TRUE(rvas.insert(site.rva).second) << "duplicate RVA 0x" << std::hex << site.rva;
    EXPECT_NE(site.name, nullptr);
    EXPECT_FALSE(std::string(site.name).empty());
    EXPECT_GT(site.rva, 0x100000U);
    EXPECT_LT(site.rva, 0x2000000U);
  }
}

TEST(CrashRecoveryN71Sites, LookupUsesNearestSiteWithinTheDocumentedSpan) {
  EXPECT_STREQ(CrashRecovery::LookupKnownNullDerefSite(0xC540A0), "DispatchEvent[0]");
  EXPECT_STREQ(CrashRecovery::LookupKnownNullDerefSite(0xC540A0 + 0x7ff), "DispatchEvent[2]");
  EXPECT_EQ(CrashRecovery::LookupKnownNullDerefSite(0x100000), nullptr);
  EXPECT_EQ(CrashRecovery::LookupKnownNullDerefSite(-1), nullptr);
}

TEST(CrashRecoveryReadableMemory, RejectsNullAndUnmappedAddresses) {
  EXPECT_FALSE(CrashRecovery::IsReadableMemory(nullptr, 1));
  EXPECT_FALSE(CrashRecovery::IsReadableMemory(reinterpret_cast<const void*>(0x1), 1));
  EXPECT_FALSE(CrashRecovery::IsReadableMemory(reinterpret_cast<const void*>(0x7fff000000000000ULL), 1));
}

TEST(CrashRecoveryReadableMemory, AcceptsOwnStackStorage) {
  const uint64_t value = 42;
  EXPECT_TRUE(CrashRecovery::IsReadableMemory(&value, sizeof(value)));
}

TEST(CrashRecoveryReadableMemory, RejectsCommittedNoAccessPages) {
  void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  ASSERT_NE(page, nullptr);
  ASSERT_TRUE(CrashRecovery::IsReadableMemory(page, 1));

  DWORD oldProtection = 0;
  const BOOL protectedPage = VirtualProtect(page, 4096, PAGE_NOACCESS, &oldProtection);
  EXPECT_NE(protectedPage, FALSE);
  if (protectedPage != FALSE) {
    EXPECT_FALSE(CrashRecovery::IsReadableMemory(page, 1));
  }
  EXPECT_NE(VirtualFree(page, 0, MEM_RELEASE), FALSE);
}

TEST(CrashDumpFormat, AccessViolationIncludesCodeAddressAndGameModule) {
  char line[256] = {};
  const int written = CrashRecovery::FormatCrashExceptionSummary(
      line, sizeof(line), 0xC0000005, 0x140123456ULL, 0x140000000ULL, 42);

  EXPECT_GT(written, 0);
  EXPECT_STREQ(line,
      "[NEVR.CRASH] exception name=ACCESS_VIOLATION code=0xC0000005 "
      "rip=0x140123456 rip_rva=game+0x123456 tid=42");
}

TEST(CrashDumpFormat, ExternalAddressAndKnownExceptionNamesAreFormattedWithoutFloats) {
  char line[256] = {};
  CrashRecovery::FormatCrashExceptionSummary(
      line, sizeof(line), 0xC0000094, 0x7fff1234ULL, 0x140000000ULL, 7);

  EXPECT_NE(std::string(line).find("INT_DIVIDE_BY_ZERO"), std::string::npos);
  EXPECT_NE(std::string(line).find("rip_rva=external:0x7FFF1234"), std::string::npos);
  EXPECT_EQ(std::string(line).find("%f"), std::string::npos);
}

// The game's own capacity check in CStackAllocator::DirectAlloc (stack_alloc_check.h, #68): the probe must
// agree with the game on exactly when it traps, and print the same number the game prints.
TEST(StackAllocCheck, FitsExactlyAtCapacityAndFailsOneUnitPast) {
  // field38 + field40 = 0x1000 end; top 0xF00; 0x100 fits exactly, 0x104 (next multiple of 4) does not.
  EXPECT_TRUE(StackAllocCheck::Check(0x800, 0x800, 0xF00, 0x100, 0).fits);
  const auto over = StackAllocCheck::Check(0x800, 0x800, 0xF00, 0x101, 0);
  EXPECT_FALSE(over.fits);
  EXPECT_EQ(over.request, 0x104U) << "rounded up to a multiple of 4, as NEG/AND 3/ADD does";
  EXPECT_EQ(over.reported, (0x800ULL - 0x800ULL) + 0xF00ULL + 0x104ULL);
}

TEST(StackAllocCheck, AlignmentMovesTheStartUp) {
  const auto r = StackAllocCheck::Check(0, 0x10000, 0x1003, 4, 0x10);
  EXPECT_EQ(r.start, 0x1010U);
  EXPECT_TRUE(r.fits);
  EXPECT_EQ(StackAllocCheck::Check(0, 0x10000, 0x1003, 4, 0).start, 0x1003U) << "align 0 leaves the top as is";
}

TEST(StackAllocCheck, AHugeRequestIsReportedTheWayTheGameLogsIt) {
  // A request far beyond the pool fails, and the printed number is (field38 - field40) + start + request.
  const auto r = StackAllocCheck::Check(0x7000'0000ULL, 0x0100'0000ULL, 0x7000'1000ULL, 674'000'000ULL, 0);
  EXPECT_FALSE(r.fits);
  EXPECT_EQ(r.reported, (0x7000'0000ULL - 0x0100'0000ULL) + 0x7000'1000ULL + 674'000'000ULL);
}
