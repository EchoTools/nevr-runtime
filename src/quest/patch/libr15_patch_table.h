// The one place a libr15 patch site is written down (ADR 0021 section 1). The build-time patcher
// (quest_patch_libr15) reads this table to write the stubs, the slot area and the credits pairs; the sentinel's
// Attach reads the same table to build each row's trampoline and to check the live words; the host tests read it to
// check every row against the real file. There is no second copy of an address or a word.
//
// Each site is typed ONCE, as its ReVault address (the address `revault_disassemble` prints for libr15.so). The
// pinned link-time vaddr is derived: `revaultVa - kRevaultBias`. That subtraction was done by hand and went wrong
// twice (#1125 and #1128 each met a headset with the wrong function); here it is code. The stock words are the words
// the pinned libr15 holds at the site, read from the file at (revaultVa - kRevaultBias) and cross-checked against
// ReVault; the patcher refuses a stock file whose words differ, naming the row.
//
// The table itself is constexpr data (no allocation, no I/O); only the extra-row type holds a std::string and a
// std::vector, because those rows are read from a file at build time.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nevr_quest::patch {

/// ReVault's libr15.so addresses are the pinned link-time vaddrs plus this bias.
inline constexpr std::uint64_t kRevaultBias = 0x100000;

/// The libr15 the table is written against: GNU build ID and SHA-256 of the file in the store APK (the digest is the
/// one `just test-quest-hooks-pinned` already pins, and was re-measured with `sha256sum` on the file extracted from
/// the store APK for this table).
inline constexpr const char* kLibR15StockBuildId = "b243509c08ce677aeb95fa348016949b3fc45230";
inline constexpr const char* kLibR15StockSha256 = "8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20";
// The reference build's own byte-patched libr15 files the patcher also takes as input (#1737): the same build ID and layout,
// other bytes where it patched. The first is that file with its package-name strings replaced by this package's; the second is
// the file as the reference build ships it.
inline constexpr const char* kLibR15ReferenceSha256[] = {
    "936aa6a3e289484742bcf23900ffa6a185e277628a1690529140cd34a26c8d67",
    "6e9db28d612019b0e5b2e2ab515e1f9bd15d1a8f8d345060160e3172864540e1",
};

constexpr std::uint64_t RoundUp(std::uint64_t value, std::uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

/// The end of the stock file's writable PT_LOAD (p_vaddr + p_memsz, `__end__`/`_end` in its .dynsym; the end of .bss).
inline constexpr std::uint64_t kStockImageEnd = 0x37d2538;
/// The slot area holds one 8-byte slot per row. It starts kSlotAreaGap bytes after the image end, at the next 64-byte
/// boundary (.bss's own alignment). The page's slack after the image end is where other patches of this libr15 keep their own
/// state words (the reference build's two at 0x37d2540 and 0x37d2560, the community patcher's latches at 0x37d2540, 0x37d2560
/// and 0x37d2570), so the gap leaves that part of the page to them. The patcher also refuses an input whose own code reaches
/// the slot area (libr15_patcher.cpp, `slot_area_referenced`).
inline constexpr std::uint64_t kSlotAlignment = 64;
inline constexpr std::uint64_t kSlotBytes = 8;
inline constexpr std::uint64_t kSlotAreaGap = 0x100;
constexpr std::uint64_t SlotAreaFor(std::uint64_t imageEnd, std::uint64_t gap) { return RoundUp(imageEnd + gap, kSlotAlignment); }
inline constexpr std::uint64_t kSlotAreaVaddr = SlotAreaFor(kStockImageEnd, kSlotAreaGap);

enum class RowKind : std::uint8_t {
  kInlineHook,   // the first 3 words become the slot stub (stub_codec.h)
  kCreditsPair,  // ADRP+ADD pairs become ADRP+LDR from the row's slot
  kWordPatch,    // words replaced in place; no slot, no trampoline (ADR section 11); only in --extra-rows
};

inline constexpr std::size_t kMaxSiteWords = 3;
inline constexpr std::size_t kMaxSitesPerRow = 4;

/// One contiguous run of stock words: the first `wordCount` words at `revaultVa`.
struct Site {
  std::uint64_t revaultVa;
  std::uint8_t wordCount;
  std::uint32_t words[kMaxSiteWords];
  constexpr std::uint64_t Vaddr() const { return revaultVa - kRevaultBias; }
};

struct Row {
  const char* id;            // `lobby_full.join`
  const char* gameFunction;  // ReVault's name for the function
  RowKind kind;
  const char* handler;       // the sentinel's handler, or the installer for the credits row
  std::uint8_t slot;         // index of the row's 8-byte slot
  std::uint8_t siteCount;
  Site sites[kMaxSitesPerRow];

  constexpr std::uint64_t SlotVaddr(std::uint64_t slotArea = kSlotAreaVaddr) const {
    return slotArea + kSlotBytes * slot;
  }
};

inline constexpr Row kRows[] = {
    {"lobby_full.join", "CR15NetClientLobby::Join", RowKind::kInlineHook, "OnJoin", 0, 1,
     {{0x1340ba8, 3, {0xd10183ffu, 0xf9001bf6u, 0xa90453f5u}}}},
    {"lobby_full.failure", "CR15NetClientLobby::LobbySessionFailureCB", RowKind::kInlineHook, "OnFailure", 1, 1,
     {{0x1340cc8, 3, {0xf81d0ffcu, 0xa90153f5u, 0xa9027bf3u}}}},
    {"lobby_full.success", "CR15NetClientLobby::LobbySessionSuccessCB", RowKind::kInlineHook, "OnSuccess", 2, 1,
     {{0x134024c, 3, {0xd10183ffu, 0xa9035bf7u, 0xa90453f5u}}}},
    {"ui_texture.attach", "CGTextureResource::Attach", RowKind::kInlineHook, "OnAttach", 3, 1,
     {{0x19e75c4, 3, {0xd103c3ffu, 0xa90d53f5u, 0xa90e7bf3u}}}},
    {"splash_skip.widget_update", "CR15VRMenuWidgetCS::Update", RowKind::kInlineHook, "WidgetUpdateHook", 4, 1,
     {{0x209a7a0, 3, {0xd10443ffu, 0x6d0b23e9u, 0xa90c6bfcu}}}},
    // CR15CreditsUICS::InitCreditsUICI (ADRP x8, ADD x8) and OnComponentDisabled (ADRP x12, ADD x12): sites 0/1 and
    // 2/3 are the two ADRP+ADD pairs (#970); both load the credits blob and share the row's slot.
    {"credits.pairs", "CR15CreditsUICS::InitCreditsUICI, CR15CreditsUICS::OnComponentDisabled", RowKind::kCreditsPair,
     "credits_text::Install", 5, 4,
     {{0x1ea1284, 1, {0xd000a008u}},
      {0x1ea1290, 1, {0x91378508u}},
      {0x1ea1d84, 1, {0xd000a00cu}},
      {0x1ea1d88, 1, {0x9137858cu}}}},
};
inline constexpr std::size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

constexpr bool TableIsWellFormed() {
  for (std::size_t i = 0; i < kRowCount; ++i) {
    const Row& row = kRows[i];
    if (row.slot != i) return false;  // slots are dense, in row order
    if (row.kind == RowKind::kWordPatch) return false;  // the public table has no word patches
    if (row.siteCount == 0 || row.siteCount > kMaxSitesPerRow) return false;
    if (row.kind == RowKind::kInlineHook && (row.siteCount != 1 || row.sites[0].wordCount != 3)) return false;
    if (row.kind == RowKind::kCreditsPair && row.siteCount != 4) return false;
    for (std::size_t s = 0; s < row.siteCount; ++s) {
      if (row.sites[s].revaultVa < kRevaultBias || (row.sites[s].revaultVa & 3) != 0) return false;
      if (row.sites[s].wordCount == 0 || row.sites[s].wordCount > kMaxSiteWords) return false;
    }
  }
  return true;
}
static_assert(TableIsWellFormed(), "libr15_patch_table.h: a row is malformed");
static_assert(kSlotAreaVaddr == 0x37d2640, "the slot area starts at the 64-byte boundary kSlotAreaGap bytes after the stock image end");
static_assert(kSlotAreaVaddr + kSlotBytes * kRowCount == 0x37d2670, "ADR 0021 section 4: the grown end is 0x37d2670");
static_assert(kSlotAreaVaddr + kSlotBytes * kRowCount <= RoundUp(kStockImageEnd, 4096), "the grown end stays inside the page the linker already maps");

/// Limits of the extra-rows file, format 1 (ADR 0021 section 11).
inline constexpr const char* kExtraRowsFormat = "nevr-libr15-extra-rows";
inline constexpr int kExtraRowsVersion = 1;
inline constexpr const char* kExtraRowIdPrefix = "x.";
inline constexpr std::size_t kExtraRowIdMaxSuffix = 62;
inline constexpr std::size_t kMaxWordsPerRow = 64;
inline constexpr std::size_t kMaxWordsPerFile = 256;
inline constexpr std::uint32_t kMaxWordOffset = 4092;

/// Four bytes as they stand in the file (memory order), the form the extra-rows file uses.
using WordBytes = std::array<std::uint8_t, 4>;

/// The A64 instruction a word's bytes encode: the little-endian value (`1f2003d5` is 0xd503201f).
constexpr std::uint32_t WordValue(const WordBytes& bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

/// One replaced word of a word-patch row: `offset` bytes from the row's address, the stock bytes and the new bytes.
struct WordEdit {
  std::uint32_t offset = 0;
  WordBytes stock{};
  WordBytes word{};
};

/// A row of the extra-rows file (kind `word_patch`, RowKind::kWordPatch). These come from `--extra-rows` only; the
/// public table has none, and the repo holds no address or word of one.
struct WordPatchRow {
  std::string id;
  std::uint64_t revaultVa = 0;
  std::vector<WordEdit> edits;
};

}  // namespace nevr_quest::patch
