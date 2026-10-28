#pragma once

// #906 / ADR 0017 D4: the CREATE GAME and JOIN VIA CODE cards on the PC main menu.
//
// The runtime serves a copy of the player's own mnu_master page with two card elements added (menu_page.h), built in
// memory when the resource loader delivers the page. This header is the pure half: what the cards are, which page
// they are for, and the decision "this buffer is the pinned stock page, so patch it". The process half
// (menu_cards.cpp) hashes the buffer, registers the resource transform and logs.
//
// What is the echovr-modkit author's finding (MIT, github.com/Dualgame/echovr-modkit, mods/menu-cards): that these
// two owner actors exist on page 24 of mnu_master with the stock tiles tile_create and tile_join, the sha256 of the
// stock page, and the sha256 his tool produced from it. The layout is confirmed against the game's own serializers
// (menu_page.h); the code is ours.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "abi/symbol_hash.h"
#include "runtime/patch/menu_page.h"

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
/// SHA-256 of the page the echovr-modkit tool writes from that stock page with these two cards. Logged next to our
/// own result so a run says whether the two builders agree; it is not a gate.
inline constexpr char kReferenceResultSha256[] = "ac3a7173bbc78c75ba22fc9ad2f7bf12003d27bdc1b8ff1d215cdd8cae97f8b5";

inline constexpr nevr_menu_page::ElementSymbols kSymbols = {
    EchoVR::CSymbol64Hash("default"), EchoVR::CSymbol64Hash("sprite"), EchoVR::CSymbol64Hash("text"), EchoVR::CSymbol64Hash("subtext"),
    EchoVR::CSymbol64Hash("tip")};

inline constexpr uint64_t kTileCreate = EchoVR::CSymbol64Hash("tile_create");
inline constexpr uint64_t kTileJoin = EchoVR::CSymbol64Hash("tile_join");
/// The actors that own the two cards' page-element records in the stock mnu_master element resource.
inline constexpr uint64_t kCreateOwner = 0xff5375e5a4dee29dULL;
inline constexpr uint64_t kJoinOwner = 0x097b785350a61c5fULL;

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

/// Registers the transform that answers every load of mnu_master (menu_cards.cpp). Client only; logs its decision.
/// Needs the resource override registry (InstallResourceOverride) to exist.
void Install();

}  // namespace nevr_menu_cards
