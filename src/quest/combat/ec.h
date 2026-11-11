/* The EC client (#2187, docs/quest/combat/ec.md, docs/adr/0046): the Quest side of the EC network client and the writer of EC's spare page
 * `re33_ec_page` (sentinel/ec_page.h), which the EC script libraries read. Namespace nevr_quest::combat::ec.
 *
 * What it does, on a combat map or the combat lobby only (see "Arena"):
 *   - the libc recvfrom / sendto slots of libr15 (thunks): a datagram of exactly 16 bytes with both EC magics and a known type is the EC
 *     network's and is not handed to the game (tokens 'V' 'P' 'T' 'O' into the page, 'K' a drone killed on the server, 'A' and 'M'
 *     counted); the busiest sendto destination of the last second is the server;
 *   - a thread that, every 5 ms, turns the page's use counters into 'V' / 'P' / 'T' / 'O' packets sent twice to the server, sends the hello
 *     'H' every 2 s (the server's relay subscription), writes the page's "the server states EchoCombat" word (+0x40) and, with the `hide`
 *     word, hides EC's three tac mods (smoke, buff, point: the tac-mod list's last three) from the list on a server that does not state it;
 *   - CR15NetGame::LobbySettingsCB(SMsg) (GLOB_DAT thunk, #2333): the server's features from the lobby settings it gives its clients
 *     (runtime/compat/server_features.h), the gate the hide follows; a session in another lobby clears them (compat/session_router.cpp);
 *   - the WriteLogV slots of libr15, libpnsovr.so and libpnsrad.so (thunks, ec_targets.h says why WriteLogV): the profile-update line
 *     "[NSUSER] sent profile update: %s" names the selected tac mod, which is stored at page +0xe80 (with the `equip` word);
 *   - ReloadEquipmentConfig / UnloadEquipmentConfig (thunks): the gate the hide holds, so the list is never read while it is rebuilt.
 * The two platform libraries load after the combat step: their hooks go in from the module-loaded observer (integration/post_load.h) when each
 * library is reported loaded, and at install for one already loaded; a library mapped again is hooked again.
 *
 *   OnNetTick       called at the end of every recvfrom on the game's own socket, by the thread that received a datagram of the server on it (up
 *                   to 4 callbacks, in registration order), on a combat map or the combat lobby only. powerboost's link / unlink sends and
 *                   ecfx's tick hang here.
 *   OnDroneKilled   the handler a first-time 'K' datagram (a drone's health ran out on the server) goes to, on the libr15 thread that called
 *                   recvfrom. dronekill registers it.
 *
 * Arena (same APK). Every hook is installed while a player is in an arena match. The scope is `InCombatScope()`: a cached atomic the ec thread
 * refreshes from combat_level (`Classify(game, lobby=true)`) at most every 250 ms, on the CR15Game the combat tick hands it; it is true only
 * while a combat map or the combat lobby is loaded, so the arena lobbies and an Echo Arena level are out of scope. Out of scope the recvfrom and
 * sendto handlers call libc and return (one acquire load and a branch above the thunk), no datagram is consumed, no net tick runs, the thread
 * sleeps 250 ms between looks and sends nothing. Two things act everywhere, on purpose: the WriteLogV handler stores a selected tac mod in
 * re33_ec_page (memory of ours that no arena code reads; a selection made in a menu must reach the combat lobby), and the gear gate takes its
 * mutex around ReloadEquipmentConfig / UnloadEquipmentConfig (an event, not a frame) so a rebuild that started before the scope changed is
 * never read half-built. `combat_scope` (F19, #2246) will replace the cache; `InCombatScope()` is the one place that changes.
 *
 * Config (`combat:` in config.yaml): `ec` (default true), `ec_words` (default `types=VPTO equip hide`): the words are `types=<letters of VPTO>`
 * (the packet types this client carries; no word, an empty list or another letter: 'T' only), `equip` / `noequip` (store the selection or
 * only log it), `hide` (cut the list on a server without EC), `loctoken=<s>` (TEST: one LOC token every s seconds, 1..600). The plugin's
 * switch files `ec.on`, `ec.off` and `ec_preset1.on` .. `ec_preset3.on` are no longer read: one found is a Warning
 * (`combat_ec_legacy_switch`) naming the key.
 *
 * Origin (rule 5 of src/quest/combat/README.md; docs/audits/2026-11-05-q15t-origin-citations.md rows 53-62, 188-209). The q15t plugin's
 * `ec.cpp` / `ec_core.h`, a port of the reference build's `runtime/native/afkxr/afk_ec.cpp` (at `1357409`: the slots :99,102, the platform
 * libraries :123-127,419-448) and `afk_ec_core.h` (:20-33 the page layout, :162-170,247 the tac mods). Its network rules are EchoCombat v2.0.0's
 * VeilNet, `plugin/veilnet.cpp`, shared with PC in `src/runtime/patch/ec_rules.h` (the header cites the lines it follows). The pins, the hidden
 * page with its exported alias, the WriteLogV hooks and the scope are ours.
 *
 * Lines (sentinel log, events `combat_ec_*`; the per-kind caps are in ec.cpp): install, hook, page, words, legacy_switch, scope, server,
 * answer, used, token, kill, sel, tacmods, skip, prelude, loc, wall, sum, platform.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <sys/socket.h>
#include <sys/types.h>

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/ec_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::ec {

using NetTickFn = void (*)() noexcept;
using DroneKilledFn = void (*)(std::uint32_t index) noexcept;

// False when the table is full or `tick` is null. Safe before ec is installed: the callback is kept and runs once the wrappers are in.
bool OnNetTick(NetTickFn tick) noexcept;
// Replaces the handler (null clears it).
void OnDroneKilled(DroneKilledFn handler) noexcept;

// The words of `combat.ec_words` when the key is absent.
inline constexpr char kDefaultWords[] = "types=VPTO equip hide";

struct Switches {
  bool on = true;                // combat.ec
  const char* words = nullptr;   // combat.ec_words; null: kDefaultWords
};

// The step on the loaded libr15. Fixed tokens: "installed", "partial" (the net pair is in, a WriteLogV or the gear gate is not), "refused",
// "off", "already_ran", "module_not_loaded", "no_build_id", "build_id_mismatch", "tick_table_full".
ModuleResult Install(const Switches& switches) noexcept;
// The same on the images `lookup` finds, with libr15's `pins` and the three log libraries' `logLibs` (kPins and kLogLib in production).
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const LogLib* logLibs, const Switches& switches) noexcept;
// True once the net pair is in.
bool Installed() noexcept;

// The scope (see "Arena"): true while a combat map or the combat lobby is loaded. A cached load; never a read of the game.
bool InCombatScope() noexcept;
// The combat tick's callback: remembers the CR15Game the scope is read from.
void Tick(std::uintptr_t game) noexcept;
// Reads the scope again when `nowMs` is 250 ms or more past the last read (the ec thread calls it). Returns the scope.
bool RefreshScope(std::uint64_t nowMs) noexcept;

// One 5 ms pass of the thread. `now100` = CLOCK_MONOTONIC in 100 ns.
void Step(std::uint64_t now100) noexcept;

// The module-loaded observer: hooks a platform library's WriteLogV slot when the library is loaded.
void OnModuleLoaded(const char* name, void* handle) noexcept;

struct Stats {
  std::uint64_t sends, passed, consumed, capped;  // sendto calls, datagrams passed to the game, EC network datagrams consumed (in scope),
                                                  // EC datagrams past the per-call cap (passed to the game, and counted)
  std::uint64_t answers, tokens, dropped, kills, medi;
  bool answered;  // the current server states EchoCombat in its lobby settings (#2333, runtime/compat/server_features.h)
  bool haveServer;
  std::uint32_t serverAddr;
  std::uint16_t serverPort;
  int serverFd;
  bool hideWrite, equipStore, listOn;
  bool settingsOn;  // the lobby settings callback is hooked (#2333)
  std::uint32_t types;
  bool logLib[kLogLibs];  // which WriteLogV hooks are in
};
Stats StatsNow() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------
void SetNoThreadForTest(bool on) noexcept;
void SetClockForTest(std::uint64_t (*clock100)()) noexcept;  // the 100 ns clock the handlers and StatsNow read
// -1: the scope is read from the game (production); 0 or 1: the scope is that, whatever is loaded.
void ForceScopeForTest(int scope) noexcept;
// The image lookup Install and the observer use; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
void SetPinsForTest(const Pins* pins, const LogLib* logLibs) noexcept;
std::uint32_t GearGenForTest() noexcept;
// Marks the net pair as in without an install (for a module that asks Installed(): ecfx_test).
void SetInstalledForTest(bool on) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::ec
