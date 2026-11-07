/* The scanner-sphere watchdog (#2184, docs/adr/0046; moved from the q15t plugin's `sensorwd`).
 *
 * What the game does. The threat scanner's sensor sphere (model record `ac2d3f9f080ba45f`, one on every player actor that carries the combat
 * ability script `mp_combat_ability_sensor`) is enabled by the script's TriggeredPing and disabled again by FinishedPing 4.0 s later. On the Quest a sphere
 * stayed drawn for minutes.
 *
 * What this does. It times each sphere enable and turns a sphere that is still enabled 6 s after its enable off again, on the game thread; a sphere it
 * turned off comes back on at the next TriggeredPing of its own actor (the script thinks the component is still enabled, so its own EnableModel(on) would
 * be a no-op). Three hooks, all through the hook manager and none a code write: the CModelCS OnComponentEnabled and OnComponentDisabled vtable words and
 * libr15's JUMP_SLOT of CncaGameSpace::SendComponentEventGlobal (the event that carries TriggeredPing), each only while it holds the game's function. The
 * code words the wrappers' reads were derived from are checked first (sensorwd_targets.h).
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants only: with `combat::Active()` false the step never runs.
 *
 * Switch. `combat.sensorwd` in config.yaml, true unless the file says false (kotorvr's build turns it on with a `sensorwd.on` file, `afk_fx.cpp:7911-7916`, and
 * gates it to combat maps by default, `afk_fx.cpp:127-137`; the plugin's `Q15T_SENSORWD` was on). No headset session of ours has yet shown it acting (the reference build's
 * combat match counts no sphere enable, no TriggeredPing and no watchdog action), so its headset row requires `combat_sensorwd_first`
 * (docs/quest/combat/sensorwd.md). The plugin's kill file `sensorwd.off` is no longer read: one found is a Warning (`combat_sensorwd_legacy_switch`).
 *
 * Arena (same APK). The watchdog CAN act in arena: the three words are generic (every model component's enable and disable, and the game's one
 * event post), and the combat ability scripts are named for combat but are not level-specific, so the sphere can exist on an arena player's actor; kotorvr's
 * echoquest-combat found the plugin's sphere hooks live in every level, Echo Arena included, and gated them to combat maps (afk_fx.cpp:127-137, "ARENA-ISO1
 * T-143"). Here the hooks are installed on the same words, and each handler's first statement asks the scope: a combat map is loaded and no Echo Arena level
 * is, as of the last look (at most 250 ms old, and not older than 1 s, else shut). In any other state the handler calls the game's function with the same
 * arguments and does nothing else: no read of game memory, no lock, no counter of its own, no line. The tick leaves the table empty outside the scope.
 *
 * KNOWN-WRONG #2186: the scope's own look is a second level answer next to combat_scope (F19; PR #2246); see Look in sensorwd.cpp for what moves when it lands.
 *
 * Frequency (ADR 0038). OnComponentEnabled and OnComponentDisabled run for every model component enable and disable of every level (not measured on a headset);
 * SendComponentEventGlobal runs for every component event of every level. The added work on those paths in arena is the thunk's counter and loads plus one
 * relaxed load of the scope. In a combat map the plugin's cost stays: a model enable or disable reads up to five words of game memory (process_vm_readv) to
 * ask whether the component is the sphere; the event path is two compares. The tick callback runs once per rendered frame: a clock read and a compare, and every
 * 250 ms a look at the loaded levels.
 *
 * Owner (ADR 0039). This file owns turning a stuck scanner sphere off. The arguments and the results of the three functions are not changed.
 *
 * Origin. kotorvr's echoquest-combat fxsensor / sensorwd: runtime/native/afkxr/afk_fx.cpp (commit c5a67e8): the constants at :6115-6120, the sphere test at
 * :6150, the enable and disable hooks, the event hook, the tick at :6204-6250 and the combat-map gate at :6137. The ring of 8 waiting pings, the sweep and the
 * log budgets are the plugin's.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_sensorwd_install","state":"on|off|refused","reason":"default|off_config|<refusal>","switch":"combat.sensorwd","scope":"combat_map",
 *             "enabled":"<GotStatus>","disabled":"<GotStatus>","event_slot":"<GotStatus>","watchdog_ms":6000,"msg":".."}  Info on and off; Error refused
 *   scope    {"event":"combat_sensorwd_scope","from":<0|1>,"to":<0|1>,"level":"0x%016llx","msg":".."}  Info, one per change of the answer
 *   first    {"event":"combat_sensorwd_first","what":"sphere_enable|sphere_disable|ping","msg":".."}  Info, once per kind: the module saw the game's sphere
 *   thread   {"event":"combat_sensorwd_thread","role":"tick|enable|disable|event","tid":<n>,..}  Info, a role seen on a thread, the first kThreadLines
 *   off      {"event":"combat_sensorwd_off","cs":"0x..","handle":"0x..","age_ms":<n>,"n":<n>,"msg":".."}  Info, a stuck sphere turned off (the first kLogLines)
 *   back_on  {"event":"combat_sensorwd_back_on","cs":..,"handle":..,"n":<n>,"msg":".."}  Info, a forced sphere turned on at its actor's next ping
 *   note     {"event":"combat_sensorwd_note","what":"table_full|ping_lost|swept","msg":".."}  Warning (Info for swept), the first kNoteLines
 *   cap      {"event":"combat_sensorwd_cap","lines":"..","cap":<n>,"msg":".."}  Warning, once per kind of line
 *   legacy   {"event":"combat_sensorwd_legacy_switch","file":"sensorwd.off","replaced_by":"combat.sensorwd: false","msg":".."}  Warning
 * A handler logs only after the game's function has returned (callback_thunk.h rule 1).
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/sensorwd_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::sensorwd {

inline constexpr std::int64_t kWatchdogMs = 6000;   // still enabled this long after its enable = stuck (FinishedPing comes at 4.0 s)
inline constexpr std::int64_t kLookEveryMs = 250;   // the tick looks at the loaded levels this often
inline constexpr std::int64_t kStaleAfterMs = 1000; // an answer older than this is not believed: the handlers pass everything through
inline constexpr std::size_t kSlots = 32;           // spheres tracked (players and bots of one match, with room)
inline constexpr std::size_t kPings = 8;            // TriggeredPings waiting for the tick

// The module's normal step: installs the three hooks and registers the tick when `configOn`, writes the install line.
ModuleResult Install(bool configOn) noexcept;
// The same on the libr15 the lookup finds, with pinned addresses `pins` (the host test supplies an image built in memory).
ModuleResult InstallAt(bool configOn, sentinel::ImageLookup lookup, const targets::Pins& pins) noexcept;

// The tick callback (combat_tick.h `TickFn`), registered by the install; exposed for the host test.
void Tick(std::uintptr_t game) noexcept;

struct Stats {
  bool on = false;                  // the three hooks are installed
  bool inScope = false;             // the last look found a combat map and no Echo Arena level
  std::uint32_t enables = 0;        // sphere enables seen (other models are not counted)
  std::uint32_t disables = 0;       // sphere disables seen
  std::uint32_t pings = 0;          // TriggeredPing events seen
  std::uint32_t finished = 0;       // FinishedPing events seen
  std::uint32_t wdOff = 0;          // stuck spheres the tick turned off
  std::uint32_t wdBack = 0;         // forced spheres the tick turned back on
  std::uint32_t tableFull = 0;      // sphere enables that found no free slot
  std::uint32_t stale = 0;          // tracked spheres dropped because their component no longer reads as a sphere
  std::uint32_t pingsLost = 0;      // TriggeredPings dropped because kPings were already waiting for the tick
  std::uint32_t tracked = 0;        // slots in use now
  std::uint32_t forced = 0;         // of those, turned off by the tick
  std::uint32_t enabled = 0;        // of those, enabled and timed
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// A millisecond clock (nullptr: CLOCK_MONOTONIC).
void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept;
// A hook called with the state lock held at stage 11 (a sphere enable or disable), 12 (the tick), 13 (a TriggeredPing), 14 (a dead sphere dropped),
// 15 (the sweep's read of the slots) and 16 (CurrentStats).
void SetHookForTest(void (*hook)(int stage) noexcept) noexcept;
// Whether the state lock is held right now.
bool LockHeldForTest() noexcept;
// The directory the legacy kill file is looked for in (nullptr restores production).
void SetLegacyDirForTest(const char* dir) noexcept;
// Forgets the hooks, the thunks, the table, the scope, the counters and the log budgets.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::sensorwd
