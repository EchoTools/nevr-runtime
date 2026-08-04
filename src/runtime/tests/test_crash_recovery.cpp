#include <gtest/gtest.h>

#include <set>
#include <string>

#include "runtime/lifecycle/readable_memory.h"
#include "runtime/lifecycle/crash_recovery_sites.h"

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
