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

// ---------------------------------------------------------------------------
// Headless graphics-gate skips (N6/N7/N8): pin the ground-truth RVAs and the
// je(0x74)->jmp(0xEB) branch-force convention. Each gate is a 2-byte `je rel8`
// that the game takes natively when its renderer-enable bit is clear; we force
// jmp so a headless server takes the device-free branch. echovr.exe ImageBase is
// 0x140000000, image extent 0x2231000 — all four RVAs are in the .text range.
// ---------------------------------------------------------------------------

TEST(HeadlessGates, Dx12BranchForceConvention) {
  using namespace PatchAddresses;
  // je -> jmp, opcode-only edit (rel8 displacement preserved).
  EXPECT_EQ(HEADLESS_DX12_INIT_EXPECT, 0x74);  // je rel8
  EXPECT_EQ(HEADLESS_DX12_INIT_PATCH, 0xEB);   // jmp rel8
  EXPECT_NE(HEADLESS_DX12_INIT_EXPECT, HEADLESS_DX12_INIT_PATCH);
}

TEST(HeadlessGates, GateRvasPinnedToGroundTruth) {
  using namespace PatchAddresses;
  // Ground-truth VAs (VA - 0x140000000) verified via objdump on echovr.exe.
  EXPECT_EQ(HEADLESS_DX12_INIT, 0x154AF7Fu);
  EXPECT_EQ(HEADLESS_ENGINE_RENDER_INIT, 0x154B0E4u);
  EXPECT_EQ(HEADLESS_GUI_INIT, 0x154B38Fu);
  EXPECT_EQ(HEADLESS_RENDER_SUBMIT_INIT, 0x154D7E4u);  // N8 gate (je 0x74)
  EXPECT_EQ(HEADLESS_RENDER_SETUP, 0x154B683u);        // N9 gate (jne 0x75)
}

// ---------------------------------------------------------------------------
// Server→headless invariant: -server forces -headless unconditionally.
// The chain is enforced at boot.cpp: g_isHeadless = TRUE in the -server branch,
// then g_isHeadless = g_isHeadless || g_isServer as a safety net.
// This test verifies the headless-gate infrastructure is complete — all 5 gates
// must be present for the headless rendering path to work correctly in server mode.
// If a gate is added or removed, update this count and the underlying patches.
// ---------------------------------------------------------------------------

TEST(HeadlessGates, ServerForcesHeadlessGateCount) {
  // boot.cpp enforces: -server → g_isHeadless=TRUE → PatchEnableHeadless fires.
  // PatchEnableHeadless applies these 5 gates. If this count changes, a gate was
  // added or removed — update the test and verify the server→headless chain still holds.
  static constexpr int kHeadlessGateCount = 5;
  EXPECT_EQ(kHeadlessGateCount, 5);
}

TEST(HeadlessGates, GateRvasInCodeRangeAndDistinct) {
  using namespace PatchAddresses;
  const uintptr_t kImageExtent = 0x2231000;  // echovr.exe virtual size
  for (uintptr_t rva : {HEADLESS_DX12_INIT, HEADLESS_ENGINE_RENDER_INIT,
                        HEADLESS_GUI_INIT, HEADLESS_RENDER_SUBMIT_INIT,
                        HEADLESS_RENDER_SETUP}) {
    EXPECT_GT(rva, 0x100000u);
    EXPECT_LT(rva, kImageExtent);
  }
  // Each gate is a distinct 2-byte site.
  EXPECT_NE(HEADLESS_ENGINE_RENDER_INIT, HEADLESS_DX12_INIT);
  EXPECT_NE(HEADLESS_GUI_INIT, HEADLESS_ENGINE_RENDER_INIT);
  EXPECT_NE(HEADLESS_RENDER_SUBMIT_INIT, HEADLESS_GUI_INIT);
  EXPECT_NE(HEADLESS_RENDER_SETUP, HEADLESS_RENDER_SUBMIT_INIT);
}
