// The three-word stub the build-time patcher writes at an inline-hook site (ADR 0021 section 3), and the two-word
// slot load that replaces a credits ADRP+ADD pair (section 8). Pure: encoders and decoders over A64 words, no
// memory access, no allocation. The patcher writes with the encoders; the sentinel's run-time checks and the host
// tests read back with the decoders, so there is one encoding of each word.
//
//   adrp x16, <page of the slot>              EncodeAdrp   (pages relative to the site's own page)
//   ldr  x16, [x16, #<slot offset in page>]   EncodeLdrX   (unsigned immediate, scaled by 8)
//   br   x16                                  EncodeBr
//
// The stub is position independent: both the ADRP and the LDR use only the site's own address and the slot's page, so
// a load bias moves site and slot together and changes nothing. Addresses here are link-time vaddrs.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_quest::patch {

inline constexpr std::size_t kStubWords = 3;
inline constexpr unsigned kStubRegister = 16;  // x16, IP0: the register the run-time patch already clobbers

/// The ADRP immediate is a signed 21-bit count of 4 KiB pages: +-2^20 pages, +-4 GiB.
inline constexpr std::int64_t kAdrpMinPages = -(std::int64_t{1} << 20);
inline constexpr std::int64_t kAdrpMaxPages = (std::int64_t{1} << 20) - 1;
/// LDR Xt, [Xn, #imm]: a 12-bit unsigned immediate scaled by 8.
inline constexpr std::uint64_t kLdrXMaxOffset = 0xFFFull * 8;

/// `adrp xRd, <page of targetVa>` as it sits at siteVa. False when rd is not a register or the target page is out of
/// ADRP range of the site.
inline bool EncodeAdrp(std::uint64_t siteVa, std::uint64_t targetVa, unsigned rd, std::uint32_t* word) noexcept {
  if (rd > 31) return false;
  const std::int64_t pages = static_cast<std::int64_t>(targetVa >> 12) - static_cast<std::int64_t>(siteVa >> 12);
  if (pages < kAdrpMinPages || pages > kAdrpMaxPages) return false;
  const std::uint32_t immhi = static_cast<std::uint32_t>((pages >> 2) & 0x7FFFF);
  const std::uint32_t immlo = static_cast<std::uint32_t>(pages & 3);
  *word = 0x90000000u | (immlo << 29) | (immhi << 5) | rd;
  return true;
}

/// The page address `word` (an ADRP at siteVa) names, and its Rd. False when the word is not an ADRP.
inline bool DecodeAdrp(std::uint32_t word, std::uint64_t siteVa, unsigned* rd, std::uint64_t* pageVa) noexcept {
  if ((word & 0x9F000000u) != 0x90000000u) return false;
  const std::uint32_t imm = (((word >> 5) & 0x7FFFFu) << 2) | ((word >> 29) & 3u);
  const std::int64_t pages = static_cast<std::int64_t>(imm ^ 0x100000u) - 0x100000;  // sign-extend 21 bits
  *rd = word & 0x1F;
  *pageVa = (siteVa & ~std::uint64_t{0xFFF}) + static_cast<std::uint64_t>(pages * 4096);
  return true;
}

/// `ldr xRt, [xRn, #byteOffset]` (unsigned immediate). False when byteOffset is not a multiple of 8 or is out of
/// range, or a register is out of range.
inline bool EncodeLdrX(unsigned rt, unsigned rn, std::uint64_t byteOffset, std::uint32_t* word) noexcept {
  if (rt > 31 || rn > 31 || (byteOffset & 7) != 0 || byteOffset > kLdrXMaxOffset) return false;
  *word = 0xF9400000u | (static_cast<std::uint32_t>(byteOffset / 8) << 10) | (rn << 5) | rt;
  return true;
}

/// The Rt, Rn and byte offset of `ldr xRt, [xRn, #imm]` (unsigned immediate). False for any other word.
inline bool DecodeLdrX(std::uint32_t word, unsigned* rt, unsigned* rn, std::uint64_t* byteOffset) noexcept {
  if ((word & 0xFFC00000u) != 0xF9400000u) return false;
  *rt = word & 0x1F;
  *rn = (word >> 5) & 0x1F;
  *byteOffset = static_cast<std::uint64_t>((word >> 10) & 0xFFFu) * 8;
  return true;
}

/// `br xRn`.
inline constexpr std::uint32_t EncodeBr(unsigned rn) noexcept { return 0xD61F0000u | ((rn & 0x1Fu) << 5); }

/// The Rn of `br xRn`. False for any other word (BLR, RET and the authenticated forms are not BR).
inline bool DecodeBr(std::uint32_t word, unsigned* rn) noexcept {
  if ((word & 0xFFFFFC1Fu) != 0xD61F0000u) return false;
  *rn = (word >> 5) & 0x1F;
  return true;
}

struct Stub {
  std::uint32_t words[kStubWords];
};

/// The stub for a site at siteVa whose slot is at slotVa. False when the slot is not 8-byte aligned (EncodeLdrX) or its
/// page is out of ADRP range.
inline bool EncodeSlotStub(std::uint64_t siteVa, std::uint64_t slotVa, Stub* stub) noexcept {
  if (!EncodeAdrp(siteVa, slotVa, kStubRegister, &stub->words[0])) return false;
  if (!EncodeLdrX(kStubRegister, kStubRegister, slotVa & 0xFFF, &stub->words[1])) return false;
  stub->words[2] = EncodeBr(kStubRegister);
  return true;
}

/// The slot address a stub at siteVa loads from. False unless the three words are exactly `adrp x16 ; ldr x16,[x16,#n] ;
/// br x16` (a stock word, a stub with another register, and a stub whose registers disagree are all refused).
inline bool DecodeSlotStub(const std::uint32_t (&words)[kStubWords], std::uint64_t siteVa, std::uint64_t* slotVa) noexcept {
  unsigned adrpRd = 0;
  std::uint64_t page = 0;
  unsigned rt = 0;
  unsigned rn = 0;
  std::uint64_t offset = 0;
  unsigned brRn = 0;
  if (!DecodeAdrp(words[0], siteVa, &adrpRd, &page)) return false;
  if (!DecodeLdrX(words[1], &rt, &rn, &offset)) return false;
  if (!DecodeBr(words[2], &brRn)) return false;
  if (adrpRd != kStubRegister || rt != kStubRegister || rn != kStubRegister || brRn != kStubRegister) return false;
  *slotVa = page + offset;
  return true;
}

}  // namespace nevr_quest::patch
