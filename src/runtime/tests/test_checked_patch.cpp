// #519: the validated, per-site binary patch routine behind the -spectatorstream NOP.
// Pure byte logic (runtime/patch/checked_patch.h); the production writer is ProcessMemcpy.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "runtime/hook/addresses.h"
#include "runtime/patch/checked_patch.h"

namespace {

using nevr_checked_patch::Result;
using nevr_checked_patch::Site;

// The bytes mode_patches.cpp expects at SPECTATORSTREAM_CHECK: a 6-byte JE.
const uint8_t kJe[6] = {0x0f, 0x84, 0xdf, 0x00, 0x00, 0x00};
const uint8_t kNop[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

bool CopyWriter(void* dest, const void* src, size_t n) {
  std::memcpy(dest, src, n);
  return true;
}

bool RefusingWriter(void*, const void*, size_t) { return false; }

bool SilentlyDroppingWriter(void*, const void*, size_t) { return true; }

// Reports success after writing only the first half: the read-back must not accept it.
bool HalfWriter(void* dest, const void* src, size_t n) {
  std::memcpy(dest, src, n / 2);
  return true;
}

Site SpectatorSite() { return Site{kJe, kNop, sizeof(kJe)}; }

TEST(CheckedPatch, AppliesWhenTheExpectedBytesArePresent) {
  uint8_t image[6];
  std::memcpy(image, kJe, sizeof(image));
  EXPECT_EQ(nevr_checked_patch::Apply(image, SpectatorSite(), CopyWriter), Result::Applied);
  EXPECT_EQ(std::memcmp(image, kNop, sizeof(kNop)), 0);
}

TEST(CheckedPatch, RefusesAndLeavesTheSiteUntouchedWhenTheExpectedBytesAreAbsent) {
  uint8_t image[6] = {0x0f, 0x85, 0xdf, 0x00, 0x00, 0x00};  // JNE, not the JE we know
  uint8_t before[6];
  std::memcpy(before, image, sizeof(image));
  EXPECT_EQ(nevr_checked_patch::Apply(image, SpectatorSite(), CopyWriter), Result::Mismatch);
  EXPECT_EQ(std::memcmp(image, before, sizeof(before)), 0);
}

TEST(CheckedPatch, AllZeroBytesAreAMismatchNotAnApply) {
  uint8_t image[6] = {};
  EXPECT_EQ(nevr_checked_patch::Apply(image, SpectatorSite(), CopyWriter), Result::Mismatch);
  EXPECT_EQ(image[0], 0);
}

TEST(CheckedPatch, SecondApplicationIsAVerifiedNoOpThatDoesNotWrite) {
  uint8_t image[6];
  std::memcpy(image, kJe, sizeof(image));
  ASSERT_EQ(nevr_checked_patch::Apply(image, SpectatorSite(), CopyWriter), Result::Applied);
  // A writer that would fail proves the second pass never reaches the write.
  EXPECT_EQ(nevr_checked_patch::Apply(image, SpectatorSite(), RefusingWriter), Result::AlreadyApplied);
  EXPECT_EQ(std::memcmp(image, kNop, sizeof(kNop)), 0);
}

TEST(CheckedPatch, ReportsAWriterFailure) {
  uint8_t image[6];
  std::memcpy(image, kJe, sizeof(image));
  EXPECT_EQ(nevr_checked_patch::Apply(image, SpectatorSite(), RefusingWriter), Result::WriteFailed);
}

TEST(CheckedPatch, ReadsBackAndReportsAWriteThatDidNotLand) {
  // The writer says "ok" and changes nothing: only the read-back can tell.
  uint8_t image[6];
  std::memcpy(image, kJe, sizeof(image));
  EXPECT_EQ(nevr_checked_patch::Apply(image, SpectatorSite(), SilentlyDroppingWriter), Result::WriteFailed);
  EXPECT_EQ(std::memcmp(image, kJe, sizeof(kJe)), 0);
}

TEST(CheckedPatch, ReadsBackAndReportsAPartialWrite) {
  uint8_t image[6];
  std::memcpy(image, kJe, sizeof(image));
  EXPECT_EQ(nevr_checked_patch::Apply(image, SpectatorSite(), HalfWriter), Result::WriteFailed);
}

TEST(CheckedPatch, SitesAreIndependentAndEachReportsItsOwnResult) {
  // [good JE][bad bytes][already NOP]: three sites, three results, in one image.
  uint8_t image[18];
  std::memcpy(image, kJe, 6);
  const uint8_t bad[6] = {0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
  std::memcpy(image + 6, bad, 6);
  std::memcpy(image + 12, kNop, 6);
  EXPECT_EQ(nevr_checked_patch::Apply(image, SpectatorSite(), CopyWriter), Result::Applied);
  EXPECT_EQ(nevr_checked_patch::Apply(image + 6, SpectatorSite(), CopyWriter), Result::Mismatch);
  EXPECT_EQ(nevr_checked_patch::Apply(image + 12, SpectatorSite(), CopyWriter), Result::AlreadyApplied);
  EXPECT_EQ(std::memcmp(image + 6, bad, 6), 0);
}

TEST(CheckedPatch, ResultNamesAreStableTokens) {
  EXPECT_STREQ(nevr_checked_patch::ResultName(Result::Applied), "applied");
  EXPECT_STREQ(nevr_checked_patch::ResultName(Result::AlreadyApplied), "already_applied");
  EXPECT_STREQ(nevr_checked_patch::ResultName(Result::Mismatch), "mismatch");
  EXPECT_STREQ(nevr_checked_patch::ResultName(Result::WriteFailed), "write_failed");
}

TEST(CheckedPatch, SpectatorStreamSiteIsExpectedThenPatchedAndSixBytes) {
  // The pair production passes to Apply(): a swap would make the JE look "already applied" and
  // the NOPs look like a mismatch, so exercise it in both directions on a fake image.
  const Site site = nevr_checked_patch::SpectatorStreamSite();
  EXPECT_EQ(site.size, 6u);
  EXPECT_EQ(std::memcmp(site.expected, kJe, 6), 0);
  EXPECT_EQ(std::memcmp(site.patched, kNop, 6), 0);

  uint8_t image[6];
  std::memcpy(image, kJe, sizeof(image));
  EXPECT_EQ(nevr_checked_patch::Apply(image, site, CopyWriter), Result::Applied);
  EXPECT_EQ(std::memcmp(image, kNop, sizeof(kNop)), 0);
  EXPECT_EQ(nevr_checked_patch::Apply(image, site, RefusingWriter), Result::AlreadyApplied);
}

TEST(CheckedPatch, SpectatorStreamSiteAddressIsTheKnownRvaAndSize) {
  // PatchSpectatorStreamCheck writes at g_GameBaseAddress + SPECTATORSTREAM_CHECK; pin both the
  // RVA (FUN_140116720 + 0x81D) and the size the shipped bytes must match.
  EXPECT_EQ(nevr_patch_addresses::SPECTATORSTREAM_CHECK, static_cast<uintptr_t>(0x116F3D));
  EXPECT_EQ(nevr_patch_addresses::SPECTATORSTREAM_CHECK_SIZE, sizeof(nevr_checked_patch::kSpectatorStreamExpected));
  EXPECT_EQ(nevr_patch_addresses::SPECTATORSTREAM_CHECK_SIZE, sizeof(nevr_checked_patch::kSpectatorStreamPatched));
}

TEST(CheckedPatch, SpectatorStreamRecordFormat) {
  char out[192];
  const uintptr_t addr = 0x140000000u + nevr_patch_addresses::SPECTATORSTREAM_CHECK;
  nevr_checked_patch::FormatSpectatorStreamRecord(out, sizeof(out), "PatchEnableServer", addr, kJe, Result::Applied);
  EXPECT_STREQ(out,
               "[NEVR.PATCH] spectatorstream_check phase=PatchEnableServer addr=0x140116f3d "
               "expected=0f84df000000 actual=0f84df000000 result=applied");

  const uint8_t found[6] = {0x0f, 0x85, 0xdf, 0x00, 0x00, 0xcc};
  nevr_checked_patch::FormatSpectatorStreamRecord(out, sizeof(out), "PatchEnableServer", addr, found,
                                                  Result::Mismatch);
  EXPECT_STREQ(out,
               "[NEVR.PATCH] spectatorstream_check phase=PatchEnableServer addr=0x140116f3d "
               "expected=0f84df000000 actual=0f85df0000cc result=mismatch");
}

}  // namespace
