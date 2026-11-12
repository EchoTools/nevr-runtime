/* The particle-renderer gate (#2188, #1631, docs/adr/0046): the one switch every `pfx*` module reads, decided once at the normal install
 * step and logged. It installs no hook, registers no tick and writes no byte; the renderer it enables is not ported (docs/quest/combat/pfx.md
 * says what is missing), so nothing reads the gate on a frame today.
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants only: with `combat::Active()` false the step never runs and the gate reads off.
 *
 * Switch. `combat.pfx` in config.yaml, true unless the file says false (the working build has the renderer on; the reference build keeps it
 * off unless its own switch file says otherwise). The plugin's kill file `pfx.off` is no longer read: one found in the plugin's data
 * directory is a Warning (`combat_pfx_legacy_switch`) that names the key replacing it.
 *
 * Arena (same APK). Arena and combat run on the same APK, so the APK does not say which one a player is in; the loaded level does. The gate
 * a consumer asks is `GateOpenIn(game)`: open only while the switch is on and the loaded levels hold a combat map and no Echo Arena level
 * (combat_level.h; the combat lobby and the lobbies arena players use are shut). A consumer takes the answer when the level changes and
 * keeps it; it does not call this per frame or per effect create (`InCombatMap` reads the game's gamespace tables with process_vm_readv).
 *
 * Frequency (ADR 0038). The install runs once per process. `GateOn` is one atomic load. `InCombatMap` is the level scan: a few
 * process_vm_readv reads, meant for a level change. No caller exists in the tree (the renderer is #1631), so today the gate adds nothing to any
 * frame, in arena or in combat; pfx_test measures the cost of both calls so the first caller starts from a number.
 *
 * Origin. The plugin's `pfx.cpp` (#1682), whose gate reconstructs the renderer switch of kotorvr's echoquest-combat repository (commit
 * c5a67e8): `On()` at native/afkxr/afk_pfx.cpp:6958, and the level gate at runtime/native/afkxr/afk_pfx.cpp:1943-1955 (docs/quest/combat/pfx.md).
 * Nothing is copied.
 *
 * Callers (#1631). The renderer's caller runs once per effect create (about 3,450 creates while a lobby loads), so it MUST NOT call `InCombatMap`
 * or `GateOpenIn` (the direct level scan, about 3 us a call) per create. It reads `GateOn()` and the combat level answer's map-only question,
 * combat_scope.h `scope::InCombatMap()` (one relaxed load and a test outside a combat map), which the scope row refreshes four times a second; its
 * module is then one of `ScopeNormal()`'s readers. `InCombatMap(game)` and `GateOpenIn(game)` here stay for a caller that asks once, at a level change.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_pfx_install","state":"on|off","reason":"default|off_config","switch":"combat.pfx","scope":"combat_map","msg":".."}
 *   legacy   {"event":"combat_pfx_legacy_switch","file":"pfx.off","replaced_by":"combat.pfx: false","msg":".."}   Warning
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"

namespace nevr_quest::combat::pfx {

enum class GateReason { kDefault, kConfigOff };

struct GateDecision {
  bool on;
  GateReason reason;
};

// The decision from the one fact, the `combat.pfx` key (pure).
GateDecision Decide(bool configOn) noexcept;

// The module's normal step: stores the decision for GateOn(), writes the install line and warns for the plugin's kill file. Installs
// (`installed` true) when the gate is on; off by the key is `off_config`.
ModuleResult Install(bool configOn) noexcept;

// False until Install has run; then the stored decision. A pfx* module does nothing while it is false.
bool GateOn() noexcept;

// True while the loaded levels of `game` hold a combat map and no Echo Arena level (never the combat lobby, never an arena lobby).
bool InCombatMap(std::uintptr_t game) noexcept;

// GateOn() and InCombatMap(game): the gate scoped to combat maps.
bool GateOpenIn(std::uintptr_t game) noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Forgets the stored decision, so GateOn() reads false again.
void ResetForTest() noexcept;
// The directory the legacy kill file is looked for in (nullptr restores production).
void SetLegacyDirForTest(const char* dir) noexcept;

}  // namespace nevr_quest::combat::pfx
