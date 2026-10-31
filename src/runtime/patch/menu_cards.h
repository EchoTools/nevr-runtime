#pragma once

// #906 / ADR 0017 D4: the CREATE GAME and JOIN VIA CODE cards on the PC main menu.
//
// The runtime serves a copy of the player's own mnu_master page with two card elements added (menu_page.h), built in
// memory when the resource loader delivers the page, and a copy of the level's transform table in which the four
// buttons stand in the order Play, Join, Create, Tutorial (menu_slots.h; #1679). This header is the pure half: what the
// cards are, which resources they are for, and the decisions "this buffer is the pinned stock page, so patch it" and "this
// buffer is the pinned stock transform table, so place the buttons". The process half (menu_cards.cpp) hashes the buffer,
// registers the resource transforms and logs.
//
// What is the echovr-modkit author's finding (MIT, github.com/Dualgame/echovr-modkit, mods/menu-cards): that these
// two owner actors exist on page 24 of mnu_master with the stock tiles tile_create and tile_join, the sha256 of the
// stock page, and the sha256 his tool produced from it. The layout is confirmed against the game's own serializers
// (menu_page.h); the code is ours.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/patch/menu_page.h"
#include "runtime/patch/menu_slots.h"

namespace nevr_menu_cards {

/// CR15UIPage2CR and the page `mnu_master` (PC build 6323983201049540). The type is the class symbol the loader
/// callback reports for the resource; the package manifest row for the same resource carries a different type id
/// (`0x4a3fe67c1ff9ccc4`), which the callback never shows.
inline constexpr uint64_t kPageType = EchoVR::CSymbol64Hash("CR15UIPage2CR");
inline constexpr uint64_t kPageName = 0xac360e41e4ede056ULL;
/// The Quest build requests its main-menu page under `mnu_master_lowspec` (type 0x43433aacbeeab166, 81160 bytes, the
/// byte count of the PC page; src/runtime/log/symcache_data.cpp names it). The name is also the name of the main-menu
/// level, so about sixty resources of other classes arrive under it; the page is the one of class CR15UIPage2CR
/// (kPageType, the same class as PC), found by headset run 2 of dev APK 5.16.2-dev.13.
inline constexpr uint64_t kQuestPageName = 0xf927772b9e2aefb1ULL;
/// The page of the main menu inside that resource.
inline constexpr uint64_t kMainMenuPage = 24;

/// SHA-256 of the stock resource, lowercase hex. Any other bytes (another build, an already patched page) are left
/// alone.
inline constexpr char kStockPageSha256[] = "af396701cfe27cb5d9fd512943b1ad732477b6399c4df5a2b36971375a6919eb";
/// SHA-256 of the stock resource in the Quest build (APK 6528386917200980), lowercase hex, as headset run 2 of dev APK
/// 5.16.2-dev.13 logged it for class 0x75e5993f80940cd0, name 0xf927772b9e2aefb1, 81160 bytes. It equals kStockPageSha256:
/// the Quest and PC pages are byte-identical, so the same cards apply to both.
inline constexpr char kQuestStockPageSha256[] = "af396701cfe27cb5d9fd512943b1ad732477b6399c4df5a2b36971375a6919eb";

inline constexpr nevr_menu_page::ElementSymbols kSymbols = {
    EchoVR::CSymbol64Hash("default"), EchoVR::CSymbol64Hash("sprite"), EchoVR::CSymbol64Hash("text"), EchoVR::CSymbol64Hash("subtext"),
    EchoVR::CSymbol64Hash("tip")};

inline constexpr uint64_t kTileCreate = EchoVR::CSymbol64Hash("tile_create");
inline constexpr uint64_t kTileJoin = EchoVR::CSymbol64Hash("tile_join");
/// The actors that own the two cards' page-element records in the stock mnu_master element resource.
inline constexpr uint64_t kCreateOwner = 0xff5375e5a4dee29dULL;
inline constexpr uint64_t kJoinOwner = 0x097b785350a61c5fULL;

/// The actors that own the stock PLAY and TUTORIALS cards of page 24 (text `PLAY` and `TUTORIALS` in the stock page).
inline constexpr uint64_t kPlayOwner = 0xa3d3ef315cb999dbULL;
inline constexpr uint64_t kTutorialOwner = 0xd763e348e835f249ULL;

/// The order of the main menu's four buttons from the left: Play, Join, Create, Tutorial. PlaceButtons gives the four
/// owners the poses of the four slots in this order. One list, used by PC and Quest alike (both call PlaceButtons).
inline std::vector<uint64_t> ButtonOrder() { return {kPlayOwner, kJoinOwner, kCreateOwner, kTutorialOwner}; }

/// The name of a button owner, for the log: "play", "join", "create", "tutorial", else "other".
inline const char* ButtonName(uint64_t owner) {
  if (owner == kPlayOwner) return "play";
  if (owner == kJoinOwner) return "join";
  if (owner == kCreateOwner) return "create";
  if (owner == kTutorialOwner) return "tutorial";
  return "other";
}

/// The two cards, in the order they are added.
inline std::vector<nevr_menu_page::Card> Cards() {
  return {
      {kCreateOwner, kTileCreate, "CREATE GAME", "", "Create a private match"},
      {kJoinOwner, kTileJoin, "JOIN VIA CODE", "", "Join a match with an invite code"},
  };
}

enum class Verdict {
  Patched,      // the stock page, with the cards added
  NotStock,     // the buffer is not the pinned stock page: left alone
  Refused,      // the stock page, but the cards could not be added (status says why)
};

inline const char* VerdictName(Verdict v) {
  switch (v) {
    case Verdict::Patched: return "patched";
    case Verdict::NotStock: return "not_stock";
    case Verdict::Refused: return "refused";
  }
  return "unknown";
}

struct Result {
  Verdict verdict = Verdict::NotStock;
  nevr_menu_page::Status status = nevr_menu_page::Status::Ok;
  std::vector<uint8_t> bytes;  // the patched page, when verdict is Patched
};

/// Decides what to serve for a buffer the loader delivered as mnu_master. `sha256Hex` is the buffer's digest and
/// `pinHex` the digest of the stock page; the cards go in only when they are equal.
inline Result Patch(const uint8_t* data, size_t size, const std::string& sha256Hex, const std::string& pinHex,
                    const std::vector<nevr_menu_page::Card>& cards) {
  Result r;
  if (sha256Hex != pinHex) return r;
  r.status = nevr_menu_page::InsertCards(data, size, kMainMenuPage, cards.data(), cards.size(), kSymbols, &r.bytes);
  r.verdict = r.status == nevr_menu_page::Status::Ok ? Verdict::Patched : Verdict::Refused;
  if (r.verdict != Verdict::Patched) r.bytes.clear();
  return r;
}

/// The level's transform table (class `CTransformCR`), which holds where each button stands (menu_slots.h). It is
/// delivered under the same names as the page: `mnu_master` on PC (kPageName) and `mnu_master_lowspec` on Quest
/// (kQuestPageName); the class symbol is the one the loader callback reports for it (headset run of 2026-10-30: 52,680
/// bytes, class 0x9c72178d05adb686, name 0xf927772b9e2aefb1).
inline constexpr uint64_t kTransformType = EchoVR::CSymbol64Hash("CTransformCR");

/// SHA-256 of the stock transform table, lowercase hex. The PC level's table (manifest type 0x92abd3e1432bf5e8) and the Quest
/// level's (0xdc6d950e3793520c) are the same 52,680 bytes, read from the two stock installs, so one pin serves both. Any other
/// bytes (another build, a table already changed) are left alone.
inline constexpr char kStockTransformSha256[] = "60737be7817aa6909525b972be8ae1cd755d083e5f4614dbf344cc72eab0dd5e";

struct SlotResult {
  Verdict verdict = Verdict::NotStock;
  nevr_menu_slots::Status status = nevr_menu_slots::Status::Ok;
  std::vector<uint8_t> bytes;                       // the transform table with the buttons placed, when verdict is Patched
  std::vector<nevr_menu_slots::Placement> before;   // where the four stood in the stock table, in ButtonOrder() order
  std::vector<nevr_menu_slots::Placement> after;    // where they stand in the bytes served, read back from those bytes
};

/// Decides what to serve for a buffer the loader delivered as the main-menu level's transform table: the stock table
/// with the four buttons in ButtonOrder() from the left, or nothing when the buffer is not the pinned table or the
/// placement does not hold.
inline SlotResult PlaceButtons(const uint8_t* data, size_t size, const std::string& sha256Hex, const std::string& pinHex) {
  SlotResult r;
  if (sha256Hex != pinHex) return r;
  const std::vector<uint64_t> order = ButtonOrder();
  r.status = nevr_menu_slots::Place(data, size, order.data(), order.size(), &r.bytes, &r.before, &r.after);
  r.verdict = r.status == nevr_menu_slots::Status::Ok ? Verdict::Patched : Verdict::Refused;
  if (r.verdict != Verdict::Patched) {
    r.bytes.clear();
    r.after.clear();
  }
  return r;
}

/// The buttons from the left, as the log writes them: `play:1@2.150,join:2@0.730,create:3@-0.730,tutorial:4@-2.150`
/// (name, slot counted from the left, x of the slot). Read left to right it is the order on the screen, so a run's log
/// says where each button stood without a screenshot; PC and Quest write the same text.
inline std::string SlotLine(const std::vector<nevr_menu_slots::Placement>& placements) {
  std::vector<nevr_menu_slots::Placement> bySlot = placements;
  std::sort(bySlot.begin(), bySlot.end(),
            [](const nevr_menu_slots::Placement& a, const nevr_menu_slots::Placement& b) { return a.slot < b.slot; });
  std::string line;
  for (const nevr_menu_slots::Placement& p : bySlot) {
    char one[64];
    std::snprintf(one, sizeof(one), "%s%s:%u@%.3f", line.empty() ? "" : ",", ButtonName(p.owner), p.slot,
                  static_cast<double>(p.x));
    line += one;
  }
  return line;
}

/// The slot (counted from the left, 1 is the leftmost) the placements give `owner`, or 0 when it is not among them.
inline unsigned SlotOf(const std::vector<nevr_menu_slots::Placement>& placements, uint64_t owner) {
  for (const nevr_menu_slots::Placement& p : placements) {
    if (p.owner == owner) return p.slot;
  }
  return 0;
}

/// Registers the transforms that answer every load of mnu_master (menu_cards.cpp). Client only; logs its decision.
/// Needs the resource override registry (InstallResourceOverride) to exist.
void Install();

}  // namespace nevr_menu_cards
