#pragma once

// #938: the nEVR section of the in-game CREDITS page.
//
// The page's text is one NUL-terminated byte blob in echovr.exe's .rdata (RVA kBlobRva), read by
// CR15CreditsUICS::Update through a cursor at component instance +0x10. Two instructions put that
// blob's address in the cursor: a 7-byte `LEA RAX,[rip+disp32]` in InitCreditsUICI and one in
// OnComponentDisabled. This header holds the pure logic for pointing both at a runtime-built buffer
// (the nEVR block, then the game's own bytes copied from memory, then the NUL), so Ready At Dawn's
// credits stay byte-for-byte complete and none of their text is stored in this repository.
//
// Pure byte logic with an injected writer, so it is unit-tested without a game image
// (src/runtime/tests/test_credits_text.cpp). Header-only and free of Windows and game headers on
// purpose, like checked_patch.h. The process side (reading the blob, allocating near the code,
// logging) is credits_text.cpp.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/patch/checked_patch.h"

namespace nevr_credits_text {

// ---- What the game holds (STATIC: read from echovr.exe, md5 c2d5261e20eb193a8f08589d1bacaad5) ----

/// RVA of the credits blob (VA 0x141C745E0 at image base 0x140000000).
inline constexpr uintptr_t kBlobRva = 0x1C745E0;
/// Bytes before the terminating NUL.
inline constexpr size_t kBlobLength = 30091;
/// SHA-256 of those bytes, lowercase hex.
inline constexpr char kBlobSha256[] = "3e1cdeaa883ff8f9a050bd4bd3e0c187e27345919ea14ef75b2c46f3295b0a3c";

/// RVAs of the two `LEA RAX,[rip+disp32]` that load the blob (InitCreditsUICI, OnComponentDisabled).
inline constexpr uintptr_t kInitSiteRva = 0x973520;
inline constexpr uintptr_t kDisabledSiteRva = 0x973AAF;
inline constexpr size_t kSiteSize = 7;
/// `48 8d 05 <disp32>`: both resolve to kBlobRva.
inline constexpr uint8_t kInitSiteExpected[kSiteSize] = {0x48, 0x8d, 0x05, 0xb9, 0x10, 0x30, 0x01};
inline constexpr uint8_t kDisabledSiteExpected[kSiteSize] = {0x48, 0x8d, 0x05, 0x2a, 0x0b, 0x30, 0x01};

// ---- Limits on the nEVR block ----

/// The game shows 64 characters of a line and truncates the rest silently (65-byte buffer), so a
/// longer line is refused here instead of being cut by the game.
inline constexpr size_t kMaxLineBytes = 64;
/// Review limit for a name or heading: above this, a reviewer is asked whether it fits the screen.
inline constexpr size_t kReviewLineBytes = 40;
/// Most lines (blank lines included) the block may hold: about 16% of the game's own 1274.
inline constexpr size_t kMaxBlockLines = 200;

enum class BlockStatus {
  Ok,
  Empty,          // no bytes: there is no nEVR section to show
  NotTerminated,  // does not end in LF: the game stops at NUL, a last line without LF is lost
  ForbiddenByte,  // NUL, CR or '|' (the game splits a line at '|' into two columns)
  LineTooLong,    // a line over kMaxLineBytes
  TooManyLines,   // more than kMaxBlockLines lines
};

inline const char* BlockStatusName(BlockStatus s) {
  switch (s) {
    case BlockStatus::Ok: return "ok";
    case BlockStatus::Empty: return "empty_block";
    case BlockStatus::NotTerminated: return "block_not_terminated";
    case BlockStatus::ForbiddenByte: return "block_forbidden_byte";
    case BlockStatus::LineTooLong: return "block_line_too_long";
    case BlockStatus::TooManyLines: return "block_too_many_lines";
  }
  return "unknown";
}

struct BlockStats {
  size_t lines = 0;         // LF count
  size_t longestLine = 0;   // bytes, LF excluded
  size_t overReview = 0;    // lines above kReviewLineBytes
};

/// Checks the block the embedded renderer produced. This is the runtime's second guard; the first
/// is the build step that renders it. Fills `stats` (when given) with what it measured, whatever
/// the verdict.
inline BlockStatus ValidateBlock(const char* block, size_t len, BlockStats* stats = nullptr) {
  BlockStats local;
  BlockStats& s = stats != nullptr ? *stats : local;
  s = BlockStats{};
  if (block == nullptr || len == 0) return BlockStatus::Empty;
  BlockStatus verdict = BlockStatus::Ok;
  size_t lineBytes = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = block[i];
    if (c == '\0' || c == '\r' || c == '|') {
      if (verdict == BlockStatus::Ok) verdict = BlockStatus::ForbiddenByte;
    }
    if (c == '\n') {
      ++s.lines;
      if (lineBytes > s.longestLine) s.longestLine = lineBytes;
      if (lineBytes > kReviewLineBytes) ++s.overReview;
      if (lineBytes > kMaxLineBytes && verdict == BlockStatus::Ok) verdict = BlockStatus::LineTooLong;
      lineBytes = 0;
    } else {
      ++lineBytes;
    }
  }
  if (verdict != BlockStatus::Ok) return verdict;
  if (lineBytes != 0 || block[len - 1] != '\n') return BlockStatus::NotTerminated;
  if (s.lines > kMaxBlockLines) return BlockStatus::TooManyLines;
  return BlockStatus::Ok;
}

enum class BlobIdentity { Match, LengthMismatch, HashMismatch, HashUnavailable };

inline const char* BlobIdentityName(BlobIdentity i) {
  switch (i) {
    case BlobIdentity::Match: return "match";
    case BlobIdentity::LengthMismatch: return "blob_length_mismatch";
    case BlobIdentity::HashMismatch: return "blob_hash_mismatch";
    case BlobIdentity::HashUnavailable: return "blob_hash_unavailable";
  }
  return "unknown";
}

/// Is what sits at kBlobRva the blob this patch was written against? `sha256Hex` is the digest of
/// the `length` bytes found there (empty when the digest could not be computed).
inline BlobIdentity IdentifyBlob(size_t length, const char* sha256Hex) {
  if (length != kBlobLength) return BlobIdentity::LengthMismatch;
  if (sha256Hex == nullptr || sha256Hex[0] == '\0') return BlobIdentity::HashUnavailable;
  return std::strcmp(sha256Hex, kBlobSha256) == 0 ? BlobIdentity::Match : BlobIdentity::HashMismatch;
}

/// buffer = block + game bytes + NUL. The game's bytes are copied verbatim.
inline std::vector<uint8_t> BuildBuffer(const char* block, size_t blockLen, const uint8_t* game, size_t gameLen) {
  std::vector<uint8_t> out;
  out.reserve(blockLen + gameLen + 1);
  out.insert(out.end(), reinterpret_cast<const uint8_t*>(block), reinterpret_cast<const uint8_t*>(block) + blockLen);
  out.insert(out.end(), game, game + gameLen);
  out.push_back(0);
  return out;
}

/// The rel32 that makes `LEA RAX,[rip+disp]` at `siteVa` (7 bytes long) load `targetVa`.
/// False when it does not fit a signed 32-bit displacement.
inline bool ComputeDisplacement(uint64_t siteVa, uint64_t targetVa, int32_t* disp) {
  const int64_t delta = static_cast<int64_t>(targetVa) - static_cast<int64_t>(siteVa + kSiteSize);
  if (delta < INT32_MIN || delta > INT32_MAX) return false;
  *disp = static_cast<int32_t>(delta);
  return true;
}

/// `48 8d 05 <disp32 little endian>`.
inline void MakePatched(int32_t disp, uint8_t out[kSiteSize]) {
  out[0] = 0x48;
  out[1] = 0x8d;
  out[2] = 0x05;
  const uint32_t u = static_cast<uint32_t>(disp);
  for (size_t i = 0; i < 4; ++i) out[3 + i] = static_cast<uint8_t>(u >> (8 * i));
}

enum class Outcome {
  Applied,         // both sites hold the new displacement
  AlreadyApplied,  // both already held it; nothing written
  Mismatch,        // a site holds neither the expected nor the new bytes; nothing written
  OutOfRange,      // the buffer is more than +-2 GiB from a site; nothing written
  WriteFailed,     // a write failed or did not read back; both sites are back to the expected bytes
};

inline const char* OutcomeName(Outcome o) {
  switch (o) {
    case Outcome::Applied: return "applied";
    case Outcome::AlreadyApplied: return "already_applied";
    case Outcome::Mismatch: return "mismatch";
    case Outcome::OutOfRange: return "out_of_range";
    case Outcome::WriteFailed: return "write_failed";
  }
  return "unknown";
}

struct SitePlan {
  uint8_t* site;                 // where the 7 bytes are
  uint64_t siteVa;               // the address the CPU will see (the displacement is relative to it)
  const uint8_t* expected;       // kSiteSize bytes the unpatched game holds
  uint8_t patched[kSiteSize];    // filled by Retarget
};

/// Points both sites at `bufferVa`, or neither. Both are checked before anything is written; if the
/// second write fails the first is put back, so the game never runs with one site retargeted and
/// the other not (the page would then start on one text and reset to the other).
inline Outcome Retarget(SitePlan& a, SitePlan& b, uint64_t bufferVa, nevr_checked_patch::WriteFn write) {
  SitePlan* const plans[2] = {&a, &b};
  for (SitePlan* p : plans) {
    int32_t disp = 0;
    if (!ComputeDisplacement(p->siteVa, bufferVa, &disp)) return Outcome::OutOfRange;
    MakePatched(disp, p->patched);
  }
  size_t already = 0;
  for (const SitePlan* p : plans) {
    if (std::memcmp(p->site, p->patched, kSiteSize) == 0) {
      ++already;
    } else if (std::memcmp(p->site, p->expected, kSiteSize) != 0) {
      return Outcome::Mismatch;
    }
  }
  if (already == 2) return Outcome::AlreadyApplied;
  // One site patched and the other not cannot come from this function; refuse rather than guess.
  if (already != 0) return Outcome::Mismatch;

  using nevr_checked_patch::Result;
  const nevr_checked_patch::Site first{a.expected, a.patched, kSiteSize};
  if (nevr_checked_patch::Apply(a.site, first, write) != Result::Applied) return Outcome::WriteFailed;
  const nevr_checked_patch::Site second{b.expected, b.patched, kSiteSize};
  if (nevr_checked_patch::Apply(b.site, second, write) != Result::Applied) {
    // Put the first site back; if even that fails the caller must treat the game as unsafe.
    write(a.site, a.expected, kSiteSize);
    return Outcome::WriteFailed;
  }
  return Outcome::Applied;
}

/// One key=value record per site: the bytes the unpatched game should hold, the bytes found there
/// BEFORE the write, and the outcome. Same shape as the spectatorstream_check record.
inline int FormatSiteRecord(char* out, size_t cap, const char* site, uint64_t va, const uint8_t* expected,
                            const uint8_t* actual, Outcome outcome) {
  char expectedHex[2 * kSiteSize + 1];
  char actualHex[2 * kSiteSize + 1];
  for (size_t i = 0; i < kSiteSize; ++i) {
    std::snprintf(expectedHex + 2 * i, 3, "%02x", expected[i]);
    std::snprintf(actualHex + 2 * i, 3, "%02x", actual[i]);
  }
  return std::snprintf(out, cap, "[NEVR.PATCH] credits_text site=%s addr=0x%llx expected=%s actual=%s result=%s", site,
                       static_cast<unsigned long long>(va), expectedHex, actualHex, OutcomeName(outcome));
}

/// The record of a built buffer, so a run shows what the page was given.
inline int FormatBuiltRecord(char* out, size_t cap, size_t blockLines, size_t blockBytes, size_t longestLine,
                             size_t overReview, size_t gameBytes, size_t totalBytes, uint64_t bufferVa) {
  return std::snprintf(out, cap,
                       "[NEVR.PATCH] credits_text built block_lines=%zu block_bytes=%zu longest_line=%zu "
                       "over_review=%zu game_bytes=%zu total_bytes=%zu buffer=0x%llx",
                       blockLines, blockBytes, longestLine, overReview, gameBytes, totalBytes,
                       static_cast<unsigned long long>(bufferVa));
}

}  // namespace nevr_credits_text
