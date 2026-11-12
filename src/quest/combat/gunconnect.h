/* Gun connect (#2185, docs/adr/0046): on Quest offline practice the local player's guns are never connected, so they do not fire.
 * CR15NetGunCS::Connect(cs, local user id) connects every gun whose owner slot is that user's; online the lobby-member event does it within a
 * frame, offline nothing does. This calls it from the game tick: for each gun component system, when (a) some local gun is unconnected
 * (inst+0xd8 != 0), (b) the connect ring is empty and (c) the local member's actor carries the combat tag, and (a)-(c) held for 2 s,
 * Connect(cs, userId) once; re-check after 1 s and retry while (a) holds, up to 10 times, then log and stop for that gun CS.
 *
 * Which APKs (ADR 0046 rule 5). The combat variants only; with `combat::Active()` false the install step never runs.
 *
 * Switch. `combat.gunconnect` in config.yaml, on unless the file says false (UPSTREAM-DEFAULTS: the reference build's `gunconnect` is on in its shipped
 * build, afk_gunconnect.cpp:9,747; the combat plugin imported in #1591 had it on). No headset log shows the module acting yet (the headset runs that loaded the plugin's
 * copy hold its start line and never a Connect), so its headset row requires `combat_gunconnect_connect`.
 * The plugin's kill file `gunconnect.off` is no longer read; one found in the plugin's data directory is a Warning (`combat_gunconnect_legacy_switch`).
 *
 * Arena (same APK). The pass acts only while a combat map or the combat lobby is loaded and no Echo Arena level is, as the combat level answer
 * last saw it (combat_scope.h `InCombatOrLobby`, a look four times a second from the game-thread tick, not believed once the tick has stopped for
 * a second); in an Echo Arena level, in the lobbies arena players use, and in every transition that still holds an arena level, the pass finds
 * nothing to do, reads nothing of the game's, calls nothing and writes no line. The module is a reader of the scope row: `combat.gunconnect` is in
 * combat_install.cpp `ScopeNormal()`'s list, and the install refuses (`scope_not_live`) when the answer cannot change. Online it does nothing in
 * any level: the lobby-member event connects the guns first, so (a) is false.
 *
 * Frequency (ADR 0038). With the switch off nothing is registered: no frame runs any of this. With it on, the tick callback runs once per rendered
 * frame and does one clock read and one compare; a pass runs at 1 Hz (4 Hz while a local gun is unconnected). In an arena level a pass is one
 * relaxed load of the scope word and a test (gunconnect_test prints the host cost); in a combat level it adds a clock read (the answer's
 * freshness) and the gun CS search. The level scan is the scope row's, made once for every reader. Not measured on a headset.
 *
 * Origin. The reference build's Fix in kotorvr's echoquest-combat repository (commit c5a67e8): the header runtime/native/afkxr/afk_gunconnect.cpp:1-24,
 * the constants :91-93, FindCs :186-218, ReadLocal :222-240, HasCombatTag :242-254, CountGuns :256-296 and FixPass :312-358. The combat plugin imported in #1591
 * reconstructed it (`gunconnect.cpp`); this file is that reconstruction. Not ported: the diagnostic and the tick-callback registry.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_gunconnect_install","result":"installed|off|refused","reason":"..","named":<0-4>,"msg":".."}   Info; Error when refused
 *   gun cs   {"event":"combat_gunconnect_gun_cs","cs":"0x..","state":"found|found, unreadable","unconnected":<n>,"local_guns":<n>}   first sighting of a gun CS
 *   waiting  {"event":"combat_gunconnect_waiting","why":".."}   one per distinct reason the local member cannot be read
 *   connect  {"event":"combat_gunconnect_connect","cs":"0x..","tries":<n>,"unconnected":<n>,"local_guns":<n>,"user":"0x..","slot":<n>,"gen":<n>,"midx":<n>,
 *             "actor_hi":"0x..","actor_lo":"0x.."}   one per Connect call
 *   connected {"event":"combat_gunconnect_connected","cs":"0x..","tries":<n>,"local_guns":<n>}
 *   gave up  {"event":"combat_gunconnect_gave_up","cs":"0x..","tries":<n>,"unconnected":<n>,"local_guns":<n>}   Warning
 *   legacy   {"event":"combat_gunconnect_legacy_switch","file":"gunconnect.off","replaced_by":"combat.gunconnect","msg":".."}   Warning
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::gunconnect {

struct Guid {  // SActorGUID: returned by value in x0/x1 (CR15NetGame::UserActor), passed by reference to HasTag
  std::uint64_t lo, hi;
};

struct Fns {
  void (*connect)(void* cs, std::uint32_t userId);                    // CR15NetGunCS::Connect(SR15NetUserId)
  Guid (*userActor)(void* net, std::uint16_t memberIndex);            // CR15NetGame::UserActor(u16) const
  bool (*hasTag)(void* game, const Guid* guid, std::uint64_t tag);    // CTagCS::HasTag(CncaGameMT*, const SActorGUID&, CSymbol64)
  std::uintptr_t tagCombatAddr;                                       // &CR15ActorUtils::kActorTagCombat (a CSymbol64)
  int named;                                                          // how many of the four were confirmed by their exported name (log only)
};

// dlsym on the loaded game library: the name's address, or null.
using SymbolLookup = void* (*)(const char* symbol);

// The pinned build's four addresses on a libr15 loaded at `base`. `lookup` null: no name lookup, fixed addresses only. A name that resolves to
// anything but its fixed address is a refusal (false, `why` is the name).
bool Resolve(std::uintptr_t base, SymbolLookup lookup, Fns* out, const char** why) noexcept;

// The module's normal step. Off (`on` false): one Info line, nothing registered. On: refuses (`scope_not_live`) when the combat level answer
// cannot change (combat_scope.h `Live`), else finds the loaded libr15, checks its build id and the pinned words, resolves the four addresses (by
// name where the game exports it) and registers the tick callback.
ModuleResult Install(bool on) noexcept;
// The same on the libr15 `lookup` finds, with `symbols` for the name check (the host test supplies an image built in memory).
ModuleResult InstallAt(sentinel::ImageLookup lookup, SymbolLookup symbols, bool on) noexcept;
// The part after the resolution: logs `installed`, stores the functions and registers the callback. A test calls it with fake functions.
ModuleResult InstallResolved(std::uintptr_t base, const Fns& fns) noexcept;

// The tick callback (combat_tick.h): runs a pass when one is due.
void Tick(std::uintptr_t game) noexcept;

// 0 not installed, 1 running, 2 off or refused.
int Phase() noexcept;

struct Stats {
  std::uint32_t passes;      // passes that ran (past the clock)
  std::uint32_t scopedOut;   // passes that found no combat level and did nothing
  std::uint32_t connects;    // Connect calls made
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

void SetClockForTest(double (*clock)()) noexcept;
void SetLegacyDirForTest(const char* dir) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::gunconnect
