/* A peer's last-received stamp that is above the broadcaster's "now" is pulled down to "now" before the stock timeout loop runs (#2178, #1625; the
 * q15t plugin's peerclamp, moved here).
 *
 * What the game does. SBroadcasterData::CleanupPeers computes age = bd[+0x478] ("now", us) - entry[+0x228] (the peer's last-received stamp) as an
 * unsigned 64-bit subtract with no clamp (peerclamp_targets.h). A stamp above "now" reads as about 2^64 us of silence, every timeout branch fires at
 * once and the new game-server peer is destroyed as "communication timeout" 0.25 s after connect (seen on a lobby-to-match switch). Its one caller
 * is CBroadcaster::Receive(uint).
 *
 * What this does. The handler walks the peer list the way the stock loop does and, for every entry whose key is not CSysNet::SAddress::kInvalid (the
 * entries the stock loop tests), lowers a stamp above "now" to "now"; then it calls the game's function. A stamp at or below "now" (every healthy
 * peer, a real silence included) is never touched, and "now" is never written. Every read is a never-faulting read; the one write is the 8-byte stamp,
 * at an address that was just read. Same thread as the stock loop; no lock, no allocation, no game call before the original.
 *
 * Arena (same APK). It writes the game's memory (the q15t plugin declared it altering gameplay, and upstream it was TEST ONLY), so it acts only while a
 * combat map or the combat lobby is loaded (combat_scope.h `SeenCombatOrLobby`: a combat map or the combat lobby was loaded at the last look and no Echo Arena
 * level was; an Echo Arena level that is loaded wins over any combat level still loaded during a transition; the lobbies arena players use never enable it).
 * The handler tests that first and, outside it, calls the game's function with the same argument and does nothing else: no read, no write, no counter, no
 * line, no level scan. The combat body is another function (`ActInScope`, not inlined), which first checks that the look is fresh (`scope::LookIsFresh()`: a
 * tick that stopped makes a true answer false after kStaleAfterMs). Even in scope the clamp changes nothing for a healthy session, because it acts only
 * when stamp > now. Known limits: the answer lags a level change by up to scope::kLookEveryMs (a quarter of a second), and a lobby-to-match switch from a
 * lobby that is not the combat lobby is not covered (not established whether the race happens there; `combat_peerclamp_clamped` and
 * `combat_peerclamp_stats` on a headset would show it).
 *
 * Which APKs (docs/adr/0046 rule 5). It runs where combat::Active() is true: the q15t and combat variants. Its switch is `combat.peerclamp` in
 * config.yaml (default true; upstream it was off unless asked, and the q15t plugin built it on). The plugin had no switch file for it.
 *
 * Install. The patched build ID, then the code words (peerclamp_targets.h), then the GOT word that holds &SAddress::kInvalid (read once; unreadable or
 * null refuses, `invalid_unreadable`), then the JUMP_SLOT, which must hold CleanupPeers itself. The scope must be live first (`scope::Live()`: the `scope` row
 * of the install table registered the refresh and the tick is installed; else `scope_not_live` and nothing is hooked). A refused install leaves the
 * game's calls as they are. It registers in combat::InstallNormal()'s table at the position the plugin installed it (last of the guards).
 *
 * Frequency (ADR 0038). CleanupPeers runs once per CBroadcaster::Receive (the rate per frame is not known: `combat_peerclamp_stats` counts it). Every call
 * pays the thunk's counter and one relaxed atomic load of the scope word. In scope a call also reads the clock (`LookIsFresh`), pays 2 + (one per peer)
 * process_vm_readv calls and the counters. Host numbers are
 * in docs/quest/combat/peerclamp.md; not measured on a headset.
 *
 * Owner (ADR 0039). This file owns the last-received stamps of the broadcaster's peers, in a combat map or the combat lobby.
 *
 * Origin. The q15t plugin's `peerclamp.cpp`, written here (imported in #1591) on the reference build's `afk_peerclamp.cpp` (Ducky's echoquest-combat;
 * docs/audits/2026-11-05-q15t-origin-citations.md rows 99-101 and 241-242: afk_peerclamp.cpp:1,39-44,50-62,55-57,83-116). The scope is the runtime's
 * (combat_scope.h, #2246).
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_peerclamp_install","result":"installed|refused|off","reason":"ok|off_config|<token>","hook":<GotStatus>,
 *             "scope":"combat_map_or_lobby","consequence":".."}  Info for installed and off, Error for refused (reason scope_not_live; pinned_word_mismatch adds "pin" (the word's name), "vaddr", "found", "expected")
 *   clamped  {"event":"combat_peerclamp_clamped","n":<clamp count>,"idx":<entry>,"stamp":"..","now":"..","diff":"..","sent":"..","consequence":".."}  Info,
 *            the first kClampLines of the process, then {"event":"combat_peerclamp_clamped_total","clamps":<n>} at every kTotalEvery-th clamp, at most kTotalLines
 *   capped   {"event":"combat_peerclamp_walk_capped","entries":4096,"consequence":".."}  Warning, once
 *   stats    {"event":"combat_peerclamp_stats","calls":<n>,"in_scope":<n>,"walked":<entries>,"clamped":<n>,"capped":<n>,"final":0|1,
 *             "consequence":".."}  Debug every kStatsEvery in-scope calls, Info once at the end of the process when installed
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/peerclamp_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::peerclamp {

inline constexpr unsigned kMaxWalk = 4096;     // entries a walk follows before it gives up (a stale `next` that loops)
inline constexpr unsigned kClampLines = 16;    // `clamped` lines written per process
inline constexpr unsigned kTotalEvery = 100;   // then a total at every this many clamps
inline constexpr unsigned kTotalLines = 8;     // at most this many totals
inline constexpr unsigned kStatsEvery = 4096;  // a Debug stats line every this many in-scope calls (a power of two)

// The words the install pins: the slot, the function it must hold, and the GOT word that holds &SAddress::kInvalid. The host test supplies a stand-in.
struct Pins {
  std::uint64_t slot, fn, invalid;
};
inline constexpr Pins kPins = {kCleanupSlotVaddr, kCleanupVaddr, kInvalidWordVaddr};

// Installs on the libr15 `lookup` finds. `on` is combat.peerclamp. Logs one combat_peerclamp_install line. A module that is off by its switch answers
// `off` (status "off_config"): nothing is wrong and nothing was done.
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept;

// The table's row: InstallAt on the loaded libr15 with the production pins.
ModuleResult Install(bool on) noexcept;

struct Stats {
  std::uint64_t calls;    // every call of CleanupPeers that reached the handler (the thunk's counter)
  std::uint64_t inScope;  // those made while a combat map or the combat lobby was loaded
  std::uint64_t walked;   // peer entries walked in scope
  std::uint64_t clamped;  // stamps pulled down
  std::uint64_t capped;   // walks that gave up at kMaxWalk
};
Stats CurrentStats() noexcept;

// The table's end-of-process step: writes the final combat_peerclamp_stats line when the guard is installed, nothing otherwise.
void NoteEnd() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// How the install finds libr15 (production: the loaded image). nullptr restores production.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// Forgets the hook, clears the thunk, the counters and the log budget.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::peerclamp
