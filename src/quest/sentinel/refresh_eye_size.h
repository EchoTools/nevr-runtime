/* The reference libr15's eye-size call route, as pure functions (#1737, collision C4).
 *
 * CVR::RefreshRTSize (libr15 0x1a709a0, 43 words) asks the headset runtime for the eye buffer size with
 * vrapi_GetSystemPropertyInt ids 5 and 6. Two words of its body are the `bl`s that make those calls: at 0x1a709f4 (id 5) and
 * 0x1a70a04 (id 6). In the stock libr15 both are `bl vrapi_GetSystemPropertyInt@plt` (stub 0xf46080). In the reference build
 * (the community's libr15 with a cave, docs/adr/0015) both are `bl 0x1a84890`, a cave that never calls the runtime and that
 * reads its own level from settings_mp_v2_quest.json. Every other word of the body is the same in both files.
 *
 * ClassifyRefreshBody tells the two apart from the bytes the live library holds, and any other body is unknown. The reference
 * words live only in this header, as read-only checks: they are not patch rows (libr15_patch_table.h keeps word addresses out
 * of the public table).
 *
 * Pure: no Android, no game header, no hook. It is built with -fno-exceptions by the host test and by the sentinel.
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "texture_quality.h"

namespace nevr_quest::texture_quality {

// What the live libr15's RefreshRTSize body is. kStock: the game's own route (the vrapi slot answers ids 5 and 6). kReference:
// the cave route (the eye-size hook must take RefreshRTSize to answer ids 5 and 6). kUnknown: neither, nothing installs.
enum class EyeInput { kStock, kReference, kUnknown };

inline const char* InputName(EyeInput input) noexcept {
  switch (input) {
    case EyeInput::kStock: return "stock";
    case EyeInput::kReference: return "reference";
    case EyeInput::kUnknown: return "unknown";
  }
  return "unknown";
}

// The index a Classification gives when the body is not the one it expects by a word, and the one for the cave's first word.
inline constexpr int kCaveBadIndex = -2;

struct Classification {
  EyeInput input;
  int badIndex;  // the body word (0..42) that differs, kCaveBadIndex for the cave's first word, -1 when none
  std::uint32_t expected;
  std::uint32_t actual;
};

// CVR::RefreshRTSize, libr15 0x1a709a0 (the pinned vaddr; the same in both files).
inline constexpr std::uint64_t kRefreshVaddr = 0x1a709a0;
inline constexpr std::size_t kRefreshBodyWords = 43;
// The two body words that differ between the files: the `bl`s to the eye-size call (index 21 is the width, 25 the height).
inline constexpr std::size_t kSiteWordIdx[2] = {21, 25};

// The stock body, word for word (the two site words are the stock `bl vrapi_GetSystemPropertyInt@plt`).
inline constexpr std::uint32_t kStockBody[kRefreshBodyWords] = {
    0xd10103ff, 0xf90013f4, 0xa9037bf3, 0xaa1f03e0, 0x97d180ac, 0xf000e988, 0xfd41f501, 0x1e260028,  // 0..7
    0x0e0c3c29, 0x2a090108, 0x340000c8, 0x0e21d821, 0x0f809020, 0x9100a3f3, 0xfd000fe0, 0x14000012,  // 8..15
    0xf000e993, 0x91154273, 0x528000a1, 0xaa1303e0, 0x3d8003e0, 0x97d355a3, 0x2a0003f4, 0x528000c1,  // 16..23
    0xaa1303e0, 0x97d3559f, 0x3dc003e2, 0x1e220280, 0x1e220001, 0x9100a3f3, 0x1e200840, 0x1e210841,  // 24..31
    0x2d0307e0, 0x9100a3e0, 0x910063e1, 0x97d348d1, 0xf9400268, 0xa9437bf3, 0xf94013f4, 0xf000e989,  // 32..39
    0xf901f128, 0x910103ff, 0xd65f03c0,                                                              // 40..42
};

// The reference build's cave (libr15 0x1a84890; its first word, and its table for the host test's pins).
inline constexpr std::uint64_t kReferenceCaveVaddr = 0x1a84890;
inline constexpr std::uint32_t kReferenceCaveWord0 = 0xa9bc67f8;
inline constexpr int kReferenceCaveIndexW[5] = {1680, 2376, 3072, 3584, 4096};
inline constexpr int kReferenceCaveIndexH[5] = {1760, 2568, 3380, 3944, 4506};

// The target of an arm64 `bl` word at `site`: site + the signed 26-bit word offset, in bytes.
inline std::uint64_t BlTarget(std::uint32_t word, std::uint64_t site) noexcept {
  std::int32_t imm = static_cast<std::int32_t>(word & 0x03ffffffU);
  if ((imm & 0x02000000) != 0) imm -= 0x04000000;
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(site) + static_cast<std::int64_t>(imm) * 4);
}

// The `bl` word at `site` that calls `target`, or 0 when it cannot reach it (only used to name the expected word in a log line).
inline std::uint32_t BlWordTo(std::uint64_t site, std::uint64_t target) noexcept {
  const std::int64_t offset = (static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site)) / 4;
  return 0x94000000U | (static_cast<std::uint32_t>(offset) & 0x03ffffffU);
}

inline bool IsBlTo(std::uint32_t word, std::uint64_t site, std::uint64_t target) noexcept {
  return (word & 0xfc000000U) == 0x94000000U && BlTarget(word, site) == target;
}

// Classifies the 43 words of the live RefreshRTSize body, read at kRefreshVaddr, and the cave's first word, read at
// kReferenceCaveVaddr. `bodyVaddr` is kRefreshVaddr for the live library (the branch targets are computed from it).
inline Classification ClassifyRefreshBody(const std::uint32_t* words, std::uint64_t bodyVaddr,
                                          std::uint32_t caveWord0) noexcept {
  for (std::size_t i = 0; i < kRefreshBodyWords; ++i) {
    if (i == kSiteWordIdx[0] || i == kSiteWordIdx[1]) continue;
    if (words[i] != kStockBody[i]) return {EyeInput::kUnknown, static_cast<int>(i), kStockBody[i], words[i]};
  }
  int stockSites = 0;
  int referenceSites = 0;
  for (std::size_t k = 0; k < 2; ++k) {
    const std::size_t idx = kSiteWordIdx[k];
    const std::uint32_t word = words[idx];
    const std::uint64_t site = bodyVaddr + 4 * static_cast<std::uint64_t>(idx);
    if (word == kStockBody[idx]) {
      ++stockSites;
    } else if (IsBlTo(word, site, kReferenceCaveVaddr)) {
      ++referenceSites;
    } else {
      return {EyeInput::kUnknown, static_cast<int>(idx), BlWordTo(site, kReferenceCaveVaddr), word};
    }
  }
  if (stockSites == 2) return {EyeInput::kStock, -1, 0, 0};
  if (referenceSites == 2) {
    if (caveWord0 != kReferenceCaveWord0) return {EyeInput::kUnknown, kCaveBadIndex, kReferenceCaveWord0, caveWord0};
    return {EyeInput::kReference, -1, 0, 0};
  }
  // One site stock, the other a cave call: the body is neither build's.
  const std::size_t second = kSiteWordIdx[1];
  return {EyeInput::kUnknown, static_cast<int>(second), kStockBody[second], words[second]};
}

// ---- the reference build's start level (his cave's rule, reconstructed) -------------------------------------------------

// The cave reads settings_mp_v2_quest.json and keeps at most this many bytes (`fread` of 0x3bff, the cave's buffer terminated
// after them), so a key that starts past the cap is not seen.
inline constexpr std::size_t kReferenceReadCap = 15359;
// The index used when the file is missing, has no key, or has no digit after the key.
inline constexpr int kReferenceDefaultIndex = 2;

// The cave's parse, exactly: the first "msaa" that ends within the cap; the digit is the first one among the 24 bytes after it
// (a digit at the 25th byte does not count); an optional '-' or spaces before it are skipped by that scan, so `-1` reads 1; the
// digits that follow are read as a number and clamped to 0..4. `sawKey` is true when the key and a digit were both found.
inline int ReferenceIndexFromSettings(const char* text, std::size_t size, bool* sawKey) noexcept {
  if (sawKey != nullptr) *sawKey = false;
  if (text == nullptr) return kReferenceDefaultIndex;
  const std::size_t limit = size < kReferenceReadCap ? size : kReferenceReadCap;
  constexpr char kKey[] = "msaa";
  constexpr std::size_t kKeyLength = 4;
  std::size_t keyAt = limit;  // limit: not found
  for (std::size_t i = 0; i + kKeyLength <= limit; ++i) {
    bool same = true;
    for (std::size_t j = 0; j < kKeyLength && same; ++j) same = text[i + j] == kKey[j];
    if (same) {
      keyAt = i;
      break;
    }
  }
  if (keyAt == limit) return kReferenceDefaultIndex;
  std::size_t p = keyAt + kKeyLength;
  // 23 advances leave p on the 24th byte after the key at most, so a digit there is the one read and one further is not.
  for (int i = 0; i < 23 && p < limit && !(text[p] >= '0' && text[p] <= '9'); ++i) ++p;
  if (p >= limit || !(text[p] >= '0' && text[p] <= '9')) return kReferenceDefaultIndex;
  long value = 0;
  for (std::size_t q = p; q < limit && text[q] >= '0' && text[q] <= '9' && value < 1000; ++q) {
    value = value * 10 + (text[q] - '0');
  }
  if (sawKey != nullptr) *sawKey = true;
  if (value > 4) value = 4;
  return static_cast<int>(value);
}

// The level his rule starts at for an index: his V=0 is our Low, so the level is kLow + V, clamped to 0..4 (kLow..kUltra).
constexpr Level LevelForReferenceIndex(int index) noexcept {
  const int clamped = index < 0 ? 0 : (index > 4 ? 4 : index);
  return static_cast<Level>(static_cast<int>(Level::kLow) + clamped);
}

// ---- the float to int step of the game's own store --------------------------------------------------------------------

// fcvtzs: truncation toward zero, NaN gives 0, and the result saturates at the int32 limits.
inline std::int32_t TruncateToInt(float f) noexcept {
  if (std::isnan(f)) return 0;
  if (f >= 2147483648.0f) return INT32_MAX;
  if (f <= -2147483648.0f) return INT32_MIN;
  return static_cast<std::int32_t>(f);
}

// CustomViewportScale's result: the two floats the game returns in s0 and s1 (only x is used by RefreshRTSize).
struct EyeVector {
  float x;
  float y;
};

}  // namespace nevr_quest::texture_quality
