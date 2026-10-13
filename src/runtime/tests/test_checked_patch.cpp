// #519: the validated, per-site binary patch routine behind the -spectatorstream NOP.
// Pure byte logic (runtime/patch/checked_patch.h); the production writer is ProcessMemcpy.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

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
  uint8_t image[6];
  std::memcpy(image, kJe, sizeof(image));
  EXPECT_EQ(nevr_checked_patch::Apply(image, SpectatorSite(), SilentlyDroppingWriter), Result::WriteFailed);
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

TEST(CheckedPatch, SpectatorStreamAddressesMatchTheKnownSite) {
  // The shipped constants: 6 bytes at +0x116F3D inside PreprocessCommandLine (+0x81D).
  EXPECT_EQ(nevr_checked_patch::kSpectatorStreamExpected[0], 0x0f);
  EXPECT_EQ(std::memcmp(nevr_checked_patch::kSpectatorStreamExpected, kJe, 6), 0);
  EXPECT_EQ(std::memcmp(nevr_checked_patch::kSpectatorStreamPatched, kNop, 6), 0);
}

}  // namespace
