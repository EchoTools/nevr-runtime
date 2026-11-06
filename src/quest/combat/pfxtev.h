/* The event-effect decision (#2188, #1642, docs/adr/0046): the six event-effect ids (GPU events that spawn child effects) the particle renderer
 * may create. A decision only: the renderer that would ask is #1631 (the pfx gate, pfx.h), which is not in the tree, so nothing calls
 * `Allows` or `AllowsIn` yet and the switch changes nothing on a headset. The id list is the particle renderer's event tier.
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants only.
 *
 * Switch. `combat.pfxtev` in config.yaml, true unless the file says false. The plugin's kill file `pfxtev.off` is no longer read: one found
 * in the plugin's data directory is a Warning (`combat_pfxtev_legacy_switch`) that names the key replacing it. (The plugin never called
 * its own `InitPfxTev`, so its file changed nothing either.)
 *
 * Arena (same APK). The event ids are combat effects. `AllowsIn(id, inCombatMap)` answers false for one of the six outside a combat map
 * whatever the switch says, so an arena match, which loads none of them, never has one created. It is the only decision call: none skips the level.
 * `inCombatMap` is the cached map-only answer taken at a level change (pfx.h "Callers"), never a scan per create. An id that is not one of the
 * six is not this switch's: true.
 *
 * Frequency (ADR 0038). The install runs once. A decision is an array scan of six words, an atomic load and, up to the 64-line cap, one
 * log line; no caller exists in the tree, so today none runs on any frame, in arena or in combat. pfxtev_test prints the cost of a decision.
 *
 * Origin. The plugin's `pfxtev.cpp` (#1683). The six ids and the switch's place in the renderer come from the event tier of kotorvr's
 * echoquest-combat repository (commit c5a67e8): `kTierIds` at native/afkxr/afk_pfx.cpp:1914, the event row at :1915-1916, and the level gate at
 * runtime/native/afkxr/afk_pfx.cpp:1943-1955 (read as text; docs/quest/combat/pfxtev.md). Nothing is copied: the ids are six symbol hashes (each
 * the CSymbol64 of an effect name, checked by pfxtev_test) and the decision is written here.
 *
 * Lines (sentinel log):
 *   install   {"event":"combat_pfxtev_install","state":"on|off","reason":"default|off_config","switch":"combat.pfxtev","msg":".."}
 *   decision  {"event":"combat_pfxtev_decision","id":"%016llx","result":"allowed|refused","reason":"switch|level","msg":".."}   one per
 *             event-id decision, 64 per process, then one Warning `combat_pfxtev_log_cap` and the rest only counted
 *   legacy    {"event":"combat_pfxtev_legacy_switch","file":"pfxtev.off","replaced_by":"combat.pfxtev: false","msg":".."}   Warning
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"

namespace nevr_quest::combat::pfxtev {

// The event-effect ids, as symbol hashes of the pack's effects.
inline constexpr std::uint64_t kIds[] = {
    0x3db472928b4d7c45ull, 0x75db6596e0af110dull, 0x76db6596e0af110dull,
    0x83976b2ec1de4a57ull, 0xb9e607923cff12f3ull, 0xbae607923cff12f3ull,
};
inline constexpr std::size_t kIdCount = sizeof kIds / sizeof kIds[0];

// The module's normal step: stores the switch, writes the install line and warns for the plugin's kill file. Until it runs the switch is on.
ModuleResult Install(bool configOn) noexcept;

// True when the renderer may create `id` given whether a combat map is loaded: an event id is refused outside one whatever the switch says; any
// other id is true. There is no call that skips the level: the answer needs `inCombatMap` (the cached map-only answer, see pfx.h "Callers").
bool AllowsIn(std::uint64_t id, bool inCombatMap) noexcept;

struct Stats {
  std::uint32_t allowed;  // event-id decisions that allowed the create
  std::uint32_t refused;  // event-id decisions that refused it
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Back to the state before Install (on, counters zero).
void ResetForTest() noexcept;
// The directory the legacy kill file is looked for in (nullptr restores production).
void SetLegacyDirForTest(const char* dir) noexcept;

}  // namespace nevr_quest::combat::pfxtev
