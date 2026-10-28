/* #966 / ADR 0017 D4: the CREATE GAME and JOIN VIA CODE cards on the Quest main menu, the twin of PR #1129 on PC.
 *
 * One GOT hook on the data slot libr15 passes to the streaming reader as its read-finished callback
 * (CResourceManager::AsyncResourceIOCallback, GLOB_DAT 0x37195b8; pinned_targets.h). A data write: no code page is
 * written (#1132). The handler looks at the callback block before the game's callback runs (menu_cards_swap.h has the
 * block layout and the rules). When the finished resource is the main-menu page (name mnu_master_lowspec, any class:
 * runtime/patch/menu_cards.h kQuestPageName; primary buffer) it hashes the bytes and, only when they are the pinned stock page (runtime/patch/menu_cards.h,
 * kQuestStockPageSha256), builds the page with the two cards added (runtime/patch/menu_page.h, shared with PC) and serves
 * a copy allocated through the game's own allocator. Everything else is passed through untouched, and the game's callback
 * always runs, with the block it was given or with the swapped bytes.
 *
 * The data is the headset's own: the cards are built in memory from the page the game delivered; nothing derived from
 * Ready At Dawn's data is stored or shipped.
 *
 * Until a headset run has logged the stock page's digest the pin is empty, so a build serves the stock page unchanged and
 * says so: `menu_cards_page` carries the digest, the byte count and `pin=unset`.
 *
 * Events (sentinel log, JSON): `menu_cards_install` once; `menu_cards_class` for the first delivery of each resource
 * class (at most kClassLines), so a run shows what the Quest calls its resource classes; `menu_cards_resource` for the first
 * delivery of each (class, name) whose primary buffer is page-sized (kBandMin..kBandMax, at most kBandLines), so the page shows
 * whatever it is called; `menu_cards_page` for each delivery of the page (class, name, byte count, digest, pin state);
 * `menu_cards_swap` / `menu_cards_skip` with the reason. The page's bytes are never logged.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h (the handler's frames are live
 * while the game's allocator and callback run). Building the page and hashing it run before any game code and are
 * marked NEVR_OUTSIDE_GAME_CALL.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::menu_cards {

// Diagnostics, bounded: the first delivery of each resource class (kClassLines classes at most), and the first delivery of
// each (class, name) whose primary buffer is as large as a UI page (kBandLines at most, sizes kBandMin..kBandMax).
inline constexpr unsigned kClassLines = 64;
inline constexpr unsigned kBandLines = 16;
inline constexpr unsigned long long kBandMin = 60000;
inline constexpr unsigned long long kBandMax = 140000;

// Installs the thunk on libr15's callback slot. One line {"event":"menu_cards_install",...} says what happened and the
// libr15 load base. True only when the slot holds the thunk.
bool Install() noexcept;

// Test support: arms the handler without touching a GOT slot, with the pin the test chooses ("" = unset) and the address
// of the 8-byte slot that holds the memory context pointer (nullptr = the game has no context).
void ArmForTest(const char* pinHex, void* const* contextSlot) noexcept;
void ResetForTest() noexcept;

struct Totals {
  unsigned long long deliveries;  // callbacks the handler saw
  unsigned long long pages;       // deliveries of the main-menu page
  unsigned long long swapped;     // pages served with the cards
};
Totals CurrentTotals() noexcept;

}  // namespace nevr_quest::menu_cards
