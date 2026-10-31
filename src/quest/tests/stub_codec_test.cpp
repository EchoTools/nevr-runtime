// Host test for the build-time stub codec (src/quest/patch/stub_codec.h), ADR 0021 test 1.
//
// The reference encodings below were not produced by the codec: the join stub is the ADR's example, and each
// vector here was disassembled with `aarch64-linux-gnu-objdump -D -b binary -m aarch64` (binutils), which read
//   d0012c90  adrp x16, 0x37d2000      f942a210  ldr x16, [x16, #1344]      d61f0200  br x16     (site 0x1240ba8)
//   f0fffff0  adrp x16, 0x1ff000       f9400610  ldr x16, [x16, #8]         d61f0200  br x16     (site 0x200000)
// Run: stub_codec_test [name-filter]

#include <cstdint>

#include "mini_test.h"
#include "quest/patch/stub_codec.h"

namespace {

using namespace nevr_quest::patch;

constexpr std::uint64_t kJoinSite = 0x1240ba8;  // lobby_full.join, pinned vaddr
constexpr std::uint64_t kSlotZero = 0x37d2540;  // the slot of the vector above: a codec vector, not the table's slot 0

TEST(TheJoinStubIsTheAdrsReferenceVector) {
  Stub stub{};
  CHECK(EncodeSlotStub(kJoinSite, kSlotZero, &stub));
  CHECK_EQ(stub.words[0], 0xd0012c90u);
  CHECK_EQ(stub.words[1], 0xf942a210u);
  CHECK_EQ(stub.words[2], 0xd61f0200u);
}

TEST(ASlotBeforeTheSiteEncodesANegativePageDelta) {
  Stub stub{};
  CHECK(EncodeSlotStub(0x200000, 0x1ff008, &stub));
  CHECK_EQ(stub.words[0], 0xf0fffff0u);
  CHECK_EQ(stub.words[1], 0xf9400610u);
  CHECK_EQ(stub.words[2], 0xd61f0200u);
}

TEST(DecodeReadsBackTheSlotOfEveryStub) {
  const std::uint64_t sites[] = {0x1240ba8, 0x1240cc8, 0x124024c, 0x18e75c4, 0x1f9a7a0, 0x200000, 0xfff0, 0xff8};
  const std::uint64_t slots[] = {0x37d2540, 0x37d2548, 0x37d2568, 0x37d2ff8, 0x1ff008, 0x8, 0x1000};
  for (const std::uint64_t site : sites) {
    for (const std::uint64_t slot : slots) {
      Stub stub{};
      CHECK(EncodeSlotStub(site, slot, &stub));
      std::uint64_t decoded = 0;
      CHECK(DecodeSlotStub(stub.words, site, &decoded));
      CHECK_EQ(decoded, slot);
    }
  }
}

TEST(DecodeAdrpResolvesTheStockCreditsWord) {
  // libr15's own `adrp x8` at pinned 0x1da1284 (file bytes 08 a0 00 d0): the page of the credits blob, 0x31a3de1's.
  unsigned rd = 99;
  std::uint64_t page = 0;
  CHECK(DecodeAdrp(0xd000a008u, 0x1da1284, &rd, &page));
  CHECK_EQ(rd, 8u);
  CHECK_EQ(page, 0x31a3000ull);
  // The second pair's `adrp x12` at 0x1da1d84.
  CHECK(DecodeAdrp(0xd000a00cu, 0x1da1d84, &rd, &page));
  CHECK_EQ(rd, 12u);
  CHECK_EQ(page, 0x31a3000ull);
}

TEST(DecodeLdrReadsTheStockAttachWord) {
  // ldr x8, [x0, #0x78] is the fourth word of CGTextureResource::Attach (libr15 file bytes 08 3c 40 f9).
  unsigned rt = 99;
  unsigned rn = 99;
  std::uint64_t off = 0;
  CHECK(DecodeLdrX(0xf9403c08u, &rt, &rn, &off));
  CHECK_EQ(rt, 8u);
  CHECK_EQ(rn, 0u);
  CHECK_EQ(off, 0x78ull);
}

TEST(AnAdrpOutOfRangeIsRefused) {
  std::uint32_t word = 0;
  CHECK(EncodeAdrp(0x0, 0xFFFFF000ull, 16, &word));          // +2^20 - 1 pages: the largest
  CHECK(!EncodeAdrp(0x0, 0x100000000ull, 16, &word));        // +2^20 pages: one past
  CHECK(EncodeAdrp(0x100000000ull, 0x0, 16, &word));         // -2^20 pages: the smallest
  CHECK(!EncodeAdrp(0x100001000ull, 0x0, 16, &word));        // -2^20 - 1 pages
  Stub stub{};
  CHECK(!EncodeSlotStub(0x0, 0x200000000ull, &stub));        // a slot 8 GiB away
  CHECK(!EncodeSlotStub(0x200000000ull, 0x40, &stub));
  CHECK(!EncodeAdrp(0x1000, 0x2000, 32, &word));             // not a register
}

TEST(ALdrOffsetThatIsNotAnEightByteMultipleOrTooLargeIsRefused) {
  std::uint32_t word = 0;
  CHECK(EncodeLdrX(16, 16, 0xFF8, &word));
  CHECK(!EncodeLdrX(16, 16, 0x8000, &word));  // one scaled step past the 12-bit field
  CHECK(!EncodeLdrX(16, 16, 0x4, &word));
  CHECK(!EncodeLdrX(16, 16, 0x7FF9, &word));
  CHECK(EncodeLdrX(16, 16, 0x7FF8, &word));   // the largest the 12-bit scaled field holds
  CHECK(!EncodeLdrX(32, 16, 0, &word));
  Stub stub{};
  CHECK(!EncodeSlotStub(0x1000, 0x2004, &stub));  // a slot that is not 8-byte aligned
}

TEST(DecodeRefusesEveryWordThatIsNotTheStub) {
  Stub good{};
  CHECK(EncodeSlotStub(kJoinSite, kSlotZero, &good));
  std::uint64_t slot = 0;
  CHECK(DecodeSlotStub(good.words, kJoinSite, &slot));
  // The stock prologue of lobby_full.join (sub sp ; str x22 ; stp x21,x20).
  const std::uint32_t stock[3] = {0xd10183ffu, 0xf9001bf6u, 0xa90453f5u};
  CHECK(!DecodeSlotStub(stock, kJoinSite, &slot));
  // A stub through another register: adrp x17, ldr x17, br x17.
  std::uint32_t other[3] = {0, 0, 0};
  CHECK(EncodeAdrp(kJoinSite, kSlotZero, 17, &other[0]));
  CHECK(EncodeLdrX(17, 17, kSlotZero & 0xFFF, &other[1]));
  other[2] = EncodeBr(17);
  CHECK(!DecodeSlotStub(other, kJoinSite, &slot));
  // The registers disagree: ldr x16,[x17,#..].
  std::uint32_t mixed[3] = {good.words[0], 0, good.words[2]};
  CHECK(EncodeLdrX(16, 17, kSlotZero & 0xFFF, &mixed[1]));
  CHECK(!DecodeSlotStub(mixed, kJoinSite, &slot));
  // BLR x16 in place of BR x16, and RET.
  std::uint32_t blr[3] = {good.words[0], good.words[1], 0xd63f0200u};
  CHECK(!DecodeSlotStub(blr, kJoinSite, &slot));
  std::uint32_t ret[3] = {good.words[0], good.words[1], 0xd65f03c0u};
  CHECK(!DecodeSlotStub(ret, kJoinSite, &slot));
  // The stub read at another site names another page: the decode is relative to the site it is read at.
  std::uint64_t moved = 0;
  CHECK(DecodeSlotStub(good.words, kJoinSite + 0x1000, &moved));
  CHECK_EQ(moved, kSlotZero + 0x1000);
  // ADR (not ADRP) first.
  std::uint32_t adr[3] = {0x10000010u, good.words[1], good.words[2]};
  CHECK(!DecodeSlotStub(adr, kJoinSite, &slot));
}

TEST(TheCreditsPairsLoadFromTheirSlotLikeBinutilsReadsThem) {
  // Disassembled with aarch64-linux-gnu-objdump -D -b binary -m aarch64: the first pair (adrp x8 at 0x1da1284) and the
  // second (adrp x12 at 0x1da1d84) both name the page 0x37d2000 and the slot 0x37d2568 (offset 1384) of the patched libr15.
  //   b000d188 adrp x8, 0x37d2000   f942b508 ldr x8, [x8, #1384]
  //   b000d18c adrp x12, 0x37d2000  f942b58c ldr x12, [x12, #1384]
  SlotLoadPair a{};
  CHECK(EncodeSlotLoadPair(0x1da1284, 0x37d2568, 8, &a));
  CHECK_EQ(a.adrp, 0xb000d188u);
  CHECK_EQ(a.ldr, 0xf942b508u);
  SlotLoadPair b{};
  CHECK(EncodeSlotLoadPair(0x1da1d84, 0x37d2568, 12, &b));
  CHECK_EQ(b.adrp, 0xb000d18cu);
  CHECK_EQ(b.ldr, 0xf942b58cu);
}

TEST(ASlotLoadPairDecodesToItsSlotAndRegister) {
  const std::uint64_t sites[] = {0x1da1284, 0x1da1d84, 0x200ffc, 0x1000};
  const std::uint64_t slots[] = {0x37d2540, 0x37d2568, 0x1ff008, 0x3000};
  for (const std::uint64_t site : sites) {
    for (const std::uint64_t slot : slots) {
      SlotLoadPair pair{};
      CHECK(EncodeSlotLoadPair(site, slot, 12, &pair));
      unsigned rd = 99;
      std::uint64_t decoded = 0;
      CHECK(DecodeSlotLoadPair(pair.adrp, pair.ldr, site, &rd, &decoded));
      CHECK_EQ(rd, 12u);
      CHECK_EQ(decoded, slot);
    }
  }
}

TEST(ASlotLoadPairIsRefusedWhenItsWordsDisagree) {
  SlotLoadPair pair{};
  CHECK(EncodeSlotLoadPair(0x1da1284, 0x37d2568, 8, &pair));
  unsigned rd = 0;
  std::uint64_t slot = 0;
  // The stock pair: ADRP then ADD, not LDR.
  CHECK(!DecodeSlotLoadPair(0xd000a008u, 0x91378508u, 0x1da1284, &rd, &slot));
  // The LDR names another register than the ADRP, or loads through another base.
  std::uint32_t other = 0;
  CHECK(EncodeLdrX(9, 8, 0x568, &other));
  CHECK(!DecodeSlotLoadPair(pair.adrp, other, 0x1da1284, &rd, &slot));
  CHECK(EncodeLdrX(8, 9, 0x568, &other));
  CHECK(!DecodeSlotLoadPair(pair.adrp, other, 0x1da1284, &rd, &slot));
  // The LDR is self-consistent (x9 through x9) but loads from another register than the ADRP produced.
  CHECK(EncodeLdrX(9, 9, 0x568, &other));
  CHECK(!DecodeSlotLoadPair(pair.adrp, other, 0x1da1284, &rd, &slot));
  // Not an ADRP first.
  CHECK(!DecodeSlotLoadPair(0x10000008u, pair.ldr, 0x1da1284, &rd, &slot));
  // An unaligned slot, a register out of range, and a page out of ADRP range are refused by the encoder.
  CHECK(!EncodeSlotLoadPair(0x1da1284, 0x37d2561, 8, &pair));
  CHECK(!EncodeSlotLoadPair(0x1da1284, 0x37d2568, 32, &pair));
  CHECK(!EncodeSlotLoadPair(0x1da1284, 0x1da1284 + (std::uint64_t{5} << 30), 8, &pair));
}

TEST(BrIsOnlyBr) {
  unsigned rn = 99;
  CHECK(DecodeBr(0xd61f0200u, &rn));
  CHECK_EQ(rn, 16u);
  CHECK(!DecodeBr(0xd63f0200u, &rn));  // BLR
  CHECK(!DecodeBr(0xd65f03c0u, &rn));  // RET
  CHECK_EQ(EncodeBr(30), 0xd61f03c0u);
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
