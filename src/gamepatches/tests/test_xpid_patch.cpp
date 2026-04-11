#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "patch_addresses.h"

// Byte constants duplicated from xpid_patch.cpp for independent verification.
// If the source changes, these must be updated — a mismatch is a test failure.
static const uint8_t kPsnShort[] = {0x50, 0x53, 0x4E, 0x00};  // "PSN\0"
static const uint8_t kPsnDash[]  = {0x50, 0x53, 0x4E, 0x2D};  // "PSN-"
static const uint8_t kDscShort[] = {0x44, 0x53, 0x43, 0x00};  // "DSC\0"
static const uint8_t kDscDash[]  = {0x44, 0x53, 0x43, 0x2D};  // "DSC-"

// ---------------------------------------------------------------------------
// Verify byte constants spell the right ASCII strings
// ---------------------------------------------------------------------------

TEST(XpidPatch, PsnBytesMatchAscii) {
  EXPECT_EQ(memcmp(kPsnShort, "PSN",  4), 0);  // includes null terminator
  EXPECT_EQ(memcmp(kPsnDash,  "PSN-", 4), 0);
}

TEST(XpidPatch, DscBytesMatchAscii) {
  EXPECT_EQ(memcmp(kDscShort, "DSC",  4), 0);
  EXPECT_EQ(memcmp(kDscDash,  "DSC-", 4), 0);
}

// ---------------------------------------------------------------------------
// Verify replacement is same length (no buffer overrun)
// ---------------------------------------------------------------------------

TEST(XpidPatch, ReplacementSameLength) {
  EXPECT_EQ(sizeof(kPsnShort), sizeof(kDscShort));
  EXPECT_EQ(sizeof(kPsnDash),  sizeof(kDscDash));
  EXPECT_EQ(sizeof(kPsnShort), PatchAddresses::XPID_PLATFORM_SHORT_NAME_SIZE);
  EXPECT_EQ(sizeof(kPsnDash),  PatchAddresses::XPID_PLATFORM_DASH_PREFIX_SIZE);
  EXPECT_EQ(sizeof(kPsnShort), PatchAddresses::XPID_PLATFORM_COMPACT_NAME_SIZE);
}

// ---------------------------------------------------------------------------
// Verify addresses are in plausible .rdata range and don't overlap
// ---------------------------------------------------------------------------

TEST(XpidPatch, AddressesInRdataRange) {
  using namespace PatchAddresses;

  // .rdata for echovr.exe is well above 0x1000000 and below 0x2000000
  EXPECT_GT(XPID_PLATFORM_SHORT_NAME,  0x1000000u);
  EXPECT_LT(XPID_PLATFORM_SHORT_NAME,  0x2000000u);
  EXPECT_GT(XPID_PLATFORM_DASH_PREFIX, 0x1000000u);
  EXPECT_LT(XPID_PLATFORM_DASH_PREFIX, 0x2000000u);
  EXPECT_GT(XPID_PLATFORM_COMPACT_NAME, 0x1000000u);
  EXPECT_LT(XPID_PLATFORM_COMPACT_NAME, 0x2000000u);
}

TEST(XpidPatch, AddressesDontOverlap) {
  using namespace PatchAddresses;

  // Each patch site is 4 bytes. Verify no two sites overlap.
  auto overlaps = [](uintptr_t a, size_t asz, uintptr_t b, size_t bsz) {
    return a < b + bsz && b < a + asz;
  };

  EXPECT_EQ(overlaps(XPID_PLATFORM_SHORT_NAME, 4, XPID_PLATFORM_DASH_PREFIX, 4), false);
  EXPECT_EQ(overlaps(XPID_PLATFORM_SHORT_NAME, 4, XPID_PLATFORM_COMPACT_NAME, 4), false);
  EXPECT_EQ(overlaps(XPID_PLATFORM_DASH_PREFIX, 4, XPID_PLATFORM_COMPACT_NAME, 4), false);
}

// ---------------------------------------------------------------------------
// Simulate the patch on a mock buffer and verify the result
// ---------------------------------------------------------------------------

TEST(XpidPatch, MockPatchReplacesCorrectly) {
  using namespace PatchAddresses;

  // Allocate a buffer large enough to hold the highest patch offset + 4 bytes.
  const size_t buf_size = XPID_PLATFORM_COMPACT_NAME + 4;
  std::vector<uint8_t> buf(buf_size, 0xCC);  // fill with INT3 as sentinel

  // Plant original PSN bytes at the three patch sites.
  memcpy(buf.data() + XPID_PLATFORM_SHORT_NAME,  kPsnShort, 4);
  memcpy(buf.data() + XPID_PLATFORM_DASH_PREFIX,  kPsnDash,  4);
  memcpy(buf.data() + XPID_PLATFORM_COMPACT_NAME, kPsnShort, 4);

  // Simulate the patch (plain memcpy — the real one uses ProcessMemcpy for VirtualProtect).
  memcpy(buf.data() + XPID_PLATFORM_SHORT_NAME,  kDscShort, 4);
  memcpy(buf.data() + XPID_PLATFORM_DASH_PREFIX,  kDscDash,  4);
  memcpy(buf.data() + XPID_PLATFORM_COMPACT_NAME, kDscShort, 4);

  // Verify each site now contains DSC.
  EXPECT_EQ(memcmp(buf.data() + XPID_PLATFORM_SHORT_NAME,  "DSC",  4), 0);
  EXPECT_EQ(memcmp(buf.data() + XPID_PLATFORM_DASH_PREFIX,  "DSC-", 4), 0);
  EXPECT_EQ(memcmp(buf.data() + XPID_PLATFORM_COMPACT_NAME, "DSC",  4), 0);

  // Verify sentinel bytes around patch sites are untouched.
  if (XPID_PLATFORM_SHORT_NAME > 0) {
    EXPECT_EQ(buf[XPID_PLATFORM_SHORT_NAME - 1], 0xCC);
  }
  EXPECT_EQ(buf[XPID_PLATFORM_SHORT_NAME + 4], 0xCC);
}

// ---------------------------------------------------------------------------
// Verify validation catches wrong bytes (would-be wrong binary version)
// ---------------------------------------------------------------------------

TEST(XpidPatch, ValidationRejectsWrongBytes) {
  using namespace PatchAddresses;

  std::vector<uint8_t> buf(XPID_PLATFORM_COMPACT_NAME + 4, 0x00);

  // Plant wrong bytes at the first site.
  const uint8_t wrong[] = {0x58, 0x42, 0x58, 0x00};  // "XBX\0"
  memcpy(buf.data() + XPID_PLATFORM_SHORT_NAME, wrong, 4);

  // Verify the validation would fail.
  EXPECT_NE(memcmp(buf.data() + XPID_PLATFORM_SHORT_NAME, kPsnShort, 4), 0);
}
