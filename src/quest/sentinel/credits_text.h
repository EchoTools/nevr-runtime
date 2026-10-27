// The project section of the in-game CREDITS page on Quest (#970): the Quest twin of the PC patch in
// src/runtime/patch/credits_text.h (N148).
//
// libr15's CR15CreditsUICS::InitCreditsUICI and CR15CreditsUICS::OnComponentDisabled each store the
// address of the credits blob into the component instance's cursor (+0x10) with an ADRP+ADD pair:
//   InitCreditsUICI:      ADRP x8 at ReVault 0x1ea1284, ADD x8 at 0x1ea1290 (x8 is stored at 0x1ea129c)
//   OnComponentDisabled:  ADRP x12 at ReVault 0x1ea1d84, ADD x12 at 0x1ea1d88 (x12 is stored at 0x1ea1d94)
// Both pairs load ReVault 0x32a3de1, the blob. The addresses below are pinned vaddrs (ReVault VA minus
// 0x100000, the convention of pinned_targets.h); the words are the pinned bytes read from libr15, little
// endian. The sentinel does not hook either function: there is no function entry to detour at a data
// reference, so the patch rewrites the two pairs to name a buffer the sentinel builds (the same shape as
// the PC patch, which rewrites the two LEA sites).
//
// This header is the pure part: the encodings and the check that a site still holds the pinned words. It
// has no libr15 access and no game headers, so the host test (tests/credits_text_test.cpp) runs it on the
// pinned constants. The process side is credits_text.cpp.
//
// Not measured on Quest (see the PR that added this file): that the blob's length and hash match the
// PC blob (src/runtime/patch/credits_text.h kBlobLength, kBlobSha256). The process side logs the hash it
// reads and does not refuse on it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_quest::credits_text {

/// The blob the two pairs load, pinned vaddr (ReVault 0x32a3de1).
inline constexpr std::uint64_t kBlobVaddr = 0x31a3de1;

/// One ADRP+ADD pair: where the two words are, the words the pinned libr15 holds, and the register.
struct PairSite {
  std::uint64_t adrpVaddr;
  std::uint64_t addVaddr;
  std::uint32_t adrpExpected;
  std::uint32_t addExpected;
};

/// InitCreditsUICI: `adrp x8` at 0x1da1284, `add x8, x8, #0xde1` at 0x1da1290.
inline constexpr PairSite kInitSite{0x1da1284, 0x1da1290, 0xd000a008, 0x91378508};
/// OnComponentDisabled: `adrp x12` at 0x1da1d84, `add x12, x12, #0xde1` at 0x1da1d88.
inline constexpr PairSite kDisabledSite{0x1da1d84, 0x1da1d88, 0xd000a00c, 0x9137858c};

/// The process side (credits_text.cpp): proves the libr15 build and the pinned words, builds the buffer, writes the
/// four words and reads them back, and logs one `credits_text` line. False, with nothing left written, on any refusal.
bool InstallCreditsText();

/// The ADRP immediate is a signed 21-bit count of 4 KiB pages: +-2^20 pages, +-4 GiB.
inline constexpr std::int64_t kAdrpMinPages = -(std::int64_t{1} << 20);
inline constexpr std::int64_t kAdrpMaxPages = (std::int64_t{1} << 20) - 1;

/// `adrp xRd, <page of targetVa>` as it sits at siteVa. False when the target page is out of ADRP range.
inline bool EncodeAdrp(std::uint64_t siteVa, std::uint64_t targetVa, std::uint32_t rd, std::uint32_t* word) {
  const std::int64_t pages =
      static_cast<std::int64_t>(targetVa >> 12) - static_cast<std::int64_t>(siteVa >> 12);
  if (pages < kAdrpMinPages || pages > kAdrpMaxPages) return false;
  const std::uint32_t immhi = static_cast<std::uint32_t>((pages >> 2) & 0x7ffff);
  const std::uint32_t immlo = static_cast<std::uint32_t>(pages & 3);
  *word = 0x90000000u | (immlo << 29) | (immhi << 5) | (rd & 0x1f);
  return true;
}

/// `add xRd, xRn, #imm12` (no shift).
inline std::uint32_t EncodeAddImm12(std::uint32_t rd, std::uint32_t rn, std::uint32_t imm12) {
  return 0x91000000u | ((imm12 & 0xfff) << 10) | ((rn & 0x1f) << 5) | (rd & 0x1f);
}

/// The two new words for one pair, pointing it at bufferVa: the ADRP to the buffer's page and the ADD
/// to its offset within the page. `imageBase` is where libr15 is loaded; the site's pinned vaddr is
/// relative to it. False when the buffer's page is out of ADRP range of the site.
struct PairWords {
  std::uint32_t adr;
  std::uint32_t add;
};

inline bool PlanPair(const PairSite& site, std::uint64_t imageBase, std::uint64_t bufferVa, PairWords* out) {
  const std::uint32_t rd = site.adrpExpected & 0x1f;
  if (!EncodeAdrp(imageBase + site.adrpVaddr, bufferVa, rd, &out->adr)) return false;
  out->add = EncodeAddImm12(rd, rd, static_cast<std::uint32_t>(bufferVa & 0xfff));
  return true;
}

/// True when the pair holds the pinned words, which are the only ones it may be rewritten from.
inline bool PairHoldsPinned(const PairSite& site, std::uint32_t adrpWord, std::uint32_t addWord) {
  return adrpWord == site.adrpExpected && addWord == site.addExpected;
}

}  // namespace nevr_quest::credits_text
