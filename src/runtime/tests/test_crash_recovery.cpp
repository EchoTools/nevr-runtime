#include <gtest/gtest.h>

#include <set>
#include <string>

#include "runtime/lifecycle/console_ctrl_policy.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/lifecycle/crash_recovery_sites.h"
#include "runtime/lifecycle/crash_dump_format.h"
#include "runtime/lifecycle/stack_alloc_check.h"
#include "runtime/lifecycle/veh_policy.h"

TEST(CrashRecoveryN71Sites, TableIsCompleteAndWellFormed) {
  EXPECT_EQ(nevr_crash_recovery::kKnownNullDerefSites.size(), 30U);
  std::set<uint64_t> rvas;
  for (const nevr_crash_recovery::KnownNullDerefSite& site : nevr_crash_recovery::kKnownNullDerefSites) {
    EXPECT_TRUE(rvas.insert(site.rva).second) << "duplicate RVA 0x" << std::hex << site.rva;
    EXPECT_NE(site.name, nullptr);
    EXPECT_FALSE(std::string(site.name).empty());
    EXPECT_GT(site.rva, 0x100000U);
    EXPECT_LT(site.rva, 0x2000000U);
  }
}

TEST(CrashRecoveryN71Sites, LookupUsesNearestSiteWithinTheDocumentedSpan) {
  EXPECT_STREQ(nevr_crash_recovery::LookupKnownNullDerefSite(0xC540A0), "DispatchEvent[0]");
  EXPECT_STREQ(nevr_crash_recovery::LookupKnownNullDerefSite(0xC540A0 + 0x7ff), "DispatchEvent[2]");
  EXPECT_EQ(nevr_crash_recovery::LookupKnownNullDerefSite(0x100000), nullptr);
  EXPECT_EQ(nevr_crash_recovery::LookupKnownNullDerefSite(-1), nullptr);
}

TEST(CrashRecoveryReadableMemory, RejectsNullAndUnmappedAddresses) {
  EXPECT_FALSE(nevr_crash_recovery::IsReadableMemory(nullptr, 1));
  EXPECT_FALSE(nevr_crash_recovery::IsReadableMemory(reinterpret_cast<const void*>(0x1), 1));
  EXPECT_FALSE(nevr_crash_recovery::IsReadableMemory(reinterpret_cast<const void*>(0x7fff000000000000ULL), 1));
}

TEST(CrashRecoveryReadableMemory, AcceptsOwnStackStorage) {
  const uint64_t value = 42;
  EXPECT_TRUE(nevr_crash_recovery::IsReadableMemory(&value, sizeof(value)));
}

TEST(CrashRecoveryReadableMemory, RejectsCommittedNoAccessPages) {
  void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  ASSERT_NE(page, nullptr);
  ASSERT_TRUE(nevr_crash_recovery::IsReadableMemory(page, 1));

  DWORD oldProtection = 0;
  const BOOL protectedPage = VirtualProtect(page, 4096, PAGE_NOACCESS, &oldProtection);
  EXPECT_NE(protectedPage, FALSE);
  if (protectedPage != FALSE) {
    EXPECT_FALSE(nevr_crash_recovery::IsReadableMemory(page, 1));
  }
  EXPECT_NE(VirtualFree(page, 0, MEM_RELEASE), FALSE);
}

TEST(CrashDumpFormat, AccessViolationIncludesCodeAddressAndGameModule) {
  char line[256] = {};
  const int written = nevr_crash_recovery::FormatCrashExceptionSummary(
      line, sizeof(line), 0xC0000005, 0x140123456ULL, 0x140000000ULL, 42);

  EXPECT_GT(written, 0);
  EXPECT_STREQ(line,
      "[NEVR.CRASH] exception name=ACCESS_VIOLATION code=0xC0000005 "
      "rip=0x140123456 rip_rva=game+0x123456 tid=42");
}

TEST(CrashDumpFormat, ExternalAddressAndKnownExceptionNamesAreFormattedWithoutFloats) {
  char line[256] = {};
  nevr_crash_recovery::FormatCrashExceptionSummary(
      line, sizeof(line), 0xC0000094, 0x7fff1234ULL, 0x140000000ULL, 7);

  EXPECT_NE(std::string(line).find("INT_DIVIDE_BY_ZERO"), std::string::npos);
  EXPECT_NE(std::string(line).find("rip_rva=external:0x7FFF1234"), std::string::npos);
  EXPECT_EQ(std::string(line).find("%f"), std::string::npos);
}

// The game's own capacity check in CStackAllocator::DirectAlloc (stack_alloc_check.h, #68): the probe must
// agree with the game on exactly when it traps, and print the same number the game prints.
TEST(StackAllocCheck, FitsExactlyAtCapacityAndFailsOneUnitPast) {
  // field38 + field40 = 0x1000 end; top 0xF00; 0x100 fits exactly, 0x104 (next multiple of 4) does not.
  EXPECT_TRUE(nevr_stack_alloc_check::Check(0x800, 0x800, 0xF00, 0x100, 0).fits);
  const auto over = nevr_stack_alloc_check::Check(0x800, 0x800, 0xF00, 0x101, 0);
  EXPECT_FALSE(over.fits);
  EXPECT_EQ(over.request, 0x104U) << "rounded up to a multiple of 4, as NEG/AND 3/ADD does";
  EXPECT_EQ(over.reported, (0x800ULL - 0x800ULL) + 0xF00ULL + 0x104ULL);
}

TEST(StackAllocCheck, AlignmentMovesTheStartUp) {
  const auto r = nevr_stack_alloc_check::Check(0, 0x10000, 0x1003, 4, 0x10);
  EXPECT_EQ(r.start, 0x1010U);
  EXPECT_TRUE(r.fits);
  EXPECT_EQ(nevr_stack_alloc_check::Check(0, 0x10000, 0x1003, 4, 0).start, 0x1003U) << "align 0 leaves the top as is";
}

TEST(StackAllocCheck, AHugeRequestIsReportedTheWayTheGameLogsIt) {
  // A request far beyond the pool fails, and the printed number is (field38 - field40) + start + request.
  const auto r = nevr_stack_alloc_check::Check(0x7000'0000ULL, 0x0100'0000ULL, 0x7000'1000ULL, 674'000'000ULL, 0);
  EXPECT_FALSE(r.fits);
  EXPECT_EQ(r.reported, (0x7000'0000ULL - 0x0100'0000ULL) + 0x7000'1000ULL + 674'000'000ULL);
}

// #241: a console event is deferred to the game's teardown only when that teardown can reach
// GameServerLib::Terminate. A server that never started GameServerLib must shut down directly.
TEST(ConsoleCtrlPolicy, DefersOnlyWhenGameHandlerIsBehindAndGameServerLibStarted) {
  EXPECT_TRUE(nevr_console_ctrl_policy::ShouldDeferToGame(true, true));
  EXPECT_FALSE(nevr_console_ctrl_policy::ShouldDeferToGame(true, false));
  EXPECT_FALSE(nevr_console_ctrl_policy::ShouldDeferToGame(false, true));
  EXPECT_FALSE(nevr_console_ctrl_policy::ShouldDeferToGame(false, false));
}

TEST(ConsoleCtrlPolicy, NoDeferReasonNamesTheMissingPrecondition) {
  EXPECT_STREQ(nevr_console_ctrl_policy::NoDeferReason(true, false),
               "GameServerLib never started, so the game teardown cannot reach Terminate");
  EXPECT_STREQ(nevr_console_ctrl_policy::NoDeferReason(false, true), "no game console handler behind ours");
}

// #339: the boot line states what InstallVEH did, per platform. The Wine client skips the handler
// and must not log "veh installed".
TEST(VehPolicy, InstallsOnServersAndNativeClientsOnly) {
  EXPECT_TRUE(nevr_veh_policy::ShouldInstall(/*isServer=*/true, /*isWineClient=*/false));
  EXPECT_TRUE(nevr_veh_policy::ShouldInstall(/*isServer=*/true, /*isWineClient=*/true));
  EXPECT_TRUE(nevr_veh_policy::ShouldInstall(/*isServer=*/false, /*isWineClient=*/false));
  EXPECT_FALSE(nevr_veh_policy::ShouldInstall(/*isServer=*/false, /*isWineClient=*/true));
}

TEST(VehPolicy, BootLineSaysInstalledOnlyWhenInstalled) {
  EXPECT_STREQ(nevr_veh_policy::BootLine(true), "[NEVR.CRASH] veh installed\n");
  EXPECT_STREQ(nevr_veh_policy::BootLine(false), "[NEVR.CRASH] veh skipped reason=wine_client\n");
}

TEST(VehPolicy, WineClientBootLineNeverClaimsInstalled) {
  const bool installed = nevr_veh_policy::ShouldInstall(/*isServer=*/false, /*isWineClient=*/true);
  EXPECT_EQ(std::string(nevr_veh_policy::BootLine(installed)).find("veh installed"), std::string::npos);
}
