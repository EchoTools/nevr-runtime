// .evrp package parsing (the layout and rules are in docs/adr/0005-cosmetics-cdn-format.md).
// The accepting vector is the format's worked example, written out byte for byte with its
// field breakdown below; every rejecting case changes exactly one rule's field of that vector.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/patch/evrp_package.h"

namespace {

// A complete 108-byte tint for symbol 0x74d228d09dc5dc86 (rwd_tint_0000's hash).
std::vector<uint8_t> WorkedExample() {
  return {
      // header: magic "EVRP", format_version 1, symbol_id, slot_type tint, 7 reserved, data_length 80
      0x45, 0x56, 0x52, 0x50, 0x01, 0x00, 0x00, 0x00, 0x86, 0xDC, 0xC5, 0x9D, 0xD0, 0x28, 0xD2, 0x74,
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00,
      // colour 0 (Main 1): 1.0 0.0 0.0 1.0
      0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
      // colour 1 (Accent 1): 0.8 0.2 0.0 1.0
      0xCD, 0xCC, 0x4C, 0x3F, 0xCD, 0xCC, 0x4C, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
      // colour 2 (Main 2): 0.6 0.0 0.0 1.0
      0x9A, 0x99, 0x19, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
      // colour 3 (Accent 2): 1.0 0.4 0.1 1.0
      0x00, 0x00, 0x80, 0x3F, 0xCD, 0xCC, 0xCC, 0x3E, 0xCD, 0xCC, 0xCC, 0x3D, 0x00, 0x00, 0x80, 0x3F,
      // colour 4 (Body): 0.2 0.2 0.2 1.0
      0xCD, 0xCC, 0x4C, 0x3E, 0xCD, 0xCC, 0x4C, 0x3E, 0xCD, 0xCC, 0x4C, 0x3E, 0x00, 0x00, 0x80, 0x3F,
  };
}

constexpr int64_t kSentinelSymbol = 0x1234;

// Parses `bytes`; on rejection the outputs must be left exactly as they were.
bool Parse(const std::vector<uint8_t>& bytes, int64_t* symbol = nullptr, Evrp::TintData* tint = nullptr) {
  int64_t s = kSentinelSymbol;
  Evrp::TintData t;
  std::memset(t.colors, 0xAB, sizeof(t.colors));
  const bool ok = Evrp::ParseTint(bytes, "test.evrp", s, t);
  if (!ok) {
    EXPECT_EQ(s, kSentinelSymbol);
    for (uint8_t b : t.colors) EXPECT_EQ(b, 0xAB);
  }
  if (symbol) *symbol = s;
  if (tint) *tint = t;
  return ok;
}

TEST(EvrpPackage, WorkedExampleIsOneHundredEightBytes) {
  EXPECT_EQ(WorkedExample().size(), Evrp::kHeaderSize + Evrp::kTintDataLength);
  EXPECT_EQ(WorkedExample().size(), 108U);
}

TEST(EvrpPackage, AcceptsTheWorkedExample) {
  int64_t symbol = 0;
  Evrp::TintData tint;
  ASSERT_TRUE(Parse(WorkedExample(), &symbol, &tint));
  EXPECT_EQ(static_cast<uint64_t>(symbol), 0x74d228d09dc5dc86ULL);

  float colors[20];
  std::memcpy(colors, tint.colors, sizeof(colors));
  const float expected[20] = {1.0f, 0.0f, 0.0f, 1.0f, 0.8f, 0.2f, 0.0f, 1.0f, 0.6f, 0.0f,
                              0.0f, 1.0f, 1.0f, 0.4f, 0.1f, 1.0f, 0.2f, 0.2f, 0.2f, 1.0f};
  for (int i = 0; i < 20; ++i) EXPECT_EQ(colors[i], expected[i]) << "float " << i;
}

TEST(EvrpPackage, AcceptsColorsOutsideZeroToOne) {
  std::vector<uint8_t> bytes = WorkedExample();
  const float hdr = 4.0f;  // HDR bloom values above 1.0 must not be rejected
  std::memcpy(&bytes[Evrp::kHeaderSize], &hdr, sizeof(hdr));
  Evrp::TintData tint;
  ASSERT_TRUE(Parse(bytes, nullptr, &tint));
  float first;
  std::memcpy(&first, tint.colors, sizeof(first));
  EXPECT_EQ(first, 4.0f);
}

TEST(EvrpPackage, RejectsAFileSmallerThanTheHeader) {
  std::vector<uint8_t> bytes = WorkedExample();
  bytes.resize(Evrp::kHeaderSize - 1);
  EXPECT_FALSE(Parse(bytes));
  EXPECT_FALSE(Parse({}));
}

TEST(EvrpPackage, RejectsBadMagicInAnyByte) {
  for (size_t i = 0; i < 4; ++i) {
    std::vector<uint8_t> bytes = WorkedExample();
    bytes[i] ^= 0x01;
    EXPECT_FALSE(Parse(bytes)) << "magic byte " << i;
  }
}

TEST(EvrpPackage, RejectsAnUnsupportedFormatVersion) {
  for (size_t i = 4; i < 8; ++i) {  // format_version is a little-endian uint32 at 0x04
    std::vector<uint8_t> bytes = WorkedExample();
    bytes[i] ^= (i == 4 ? 0x03 : 0x01);  // byte 4: 1 -> 2; the high bytes: 0 -> 1
    EXPECT_FALSE(Parse(bytes)) << "version byte " << i;
  }
  std::vector<uint8_t> zero = WorkedExample();
  zero[4] = 0;
  EXPECT_FALSE(Parse(zero));
}

TEST(EvrpPackage, RejectsAnUnknownSlotType) {
  for (uint8_t slot : {uint8_t{0x00}, uint8_t{0x02}, uint8_t{0xFF}}) {
    std::vector<uint8_t> bytes = WorkedExample();
    bytes[0x10] = slot;
    EXPECT_FALSE(Parse(bytes)) << "slot " << int(slot);
  }
}

TEST(EvrpPackage, RejectsANonzeroReservedByteAtEveryPosition) {
  for (size_t i = 0x11; i < 0x18; ++i) {
    std::vector<uint8_t> bytes = WorkedExample();
    bytes[i] = 0x01;
    EXPECT_FALSE(Parse(bytes)) << "reserved byte at 0x" << std::hex << i;
  }
}

TEST(EvrpPackage, RejectsATintDataLengthOtherThanEighty) {
  // The file size is kept consistent with data_length, so only the tint-length rule can fire.
  for (uint32_t length : {79U, 81U}) {
    std::vector<uint8_t> bytes = WorkedExample();
    bytes.resize(Evrp::kHeaderSize + length, 0);
    std::memcpy(&bytes[0x18], &length, sizeof(length));
    EXPECT_FALSE(Parse(bytes)) << "data_length " << length;
  }
}

TEST(EvrpPackage, RejectsATotalSizeThatDisagreesWithDataLength) {
  std::vector<uint8_t> trailing = WorkedExample();
  trailing.push_back(0);
  EXPECT_FALSE(Parse(trailing));

  std::vector<uint8_t> truncated = WorkedExample();
  truncated.pop_back();
  EXPECT_FALSE(Parse(truncated));
}

}  // namespace
