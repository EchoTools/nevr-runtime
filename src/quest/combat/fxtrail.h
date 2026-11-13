/* A bullet whose trail effect has no instance ends its trail, so the bullet can be freed (#2178, #1737).
 *
 * What the game does. A bullet is freed only once every effect slot of it reports done; a trail slot is done when
 * CParticleEffectCS::EffectTime(handle) is above the trail's time after impact (FXUpdateTrail, the function's one caller in libr15). The Quest
 * build creates no particle effect, so the effect has no instance and EffectTime answers 0.0 (fxtrail_targets.h, the disassembly): no trail slot
 * ever finishes, no bullet with a trail is ever freed, and CR15NetBulletCS::UpdateFX queues one deferred decal-disable call per bullet per
 * frame until the queue overflows (SIGTRAP "CDeferredMethodQueue buffer has exceeded max number of calls (960)", about 53 s into a combat match).
 * The premise (that the Quest build creates no particle effect) is the reference build's claim, not re-measured here.
 *
 * What this does. The handler calls the game's EffectTime first and keeps its value, then repeats the lookup EffectTime itself makes (the index
 * table at +0xc8, the data array at +0xf8, the entry's instance at +0x10) with never-faulting reads. For an effect with no instance it answers
 * FLT_MAX: a trail that never existed has run its course. A read that fails, or an effect with an instance, answers the game's own value.
 *
 * Arena (same APK). It acts only while a combat map is loaded (combat_scope.h `SeenCombatMap`: a combat map was loaded at the last look and no Echo Arena
 * level was). The handler tests that first and, outside a combat map, calls the game's function with the same arguments and does nothing else: no read of
 * the game's memory, no counter, no line, no level scan. The combat body is another function (`ActInCombatMap`, not inlined), which first checks that the look
 * is fresh (`scope::LookIsFresh()`: a tick that stopped makes a true answer false after kStaleAfterMs). So an arena match on the combat APK costs this hook its
 * call through the thunk and, in the handler, a load of the scope word, a test and the tail call (arm64, from the object file: see the cost in
 * docs/quest/combat/fxtrail.md). The guard changes a value the game computes (a trail that "never ran its course" now has), and the fault it prevents is one
 * of bullets, which only combat has; that is why it is scoped and the crash-only guards are not.
 * The answer lags a level change by up to scope::kLookEveryMs (a quarter of a second): the leak it guards is one of a match, not of its first quarter second.
 *
 * Which APKs (docs/adr/0046 rule 5). It runs where combat::Active() is true: the combat variants. Its switch is `combat.fxtrail` in
 * config.yaml (default true). The `fxtrail.off` file is no longer read; one found in the data directory (legacy_switch.h) is a Warning
 * that names the key.
 *
 * Install. The scope must be live (`scope::Live()`: the `scope` row of the install table registered the refresh and the tick is installed; else `scope_not_live`
 * and nothing is hooked, because the guard would never see a combat map), the patched build ID, the code words (fxtrail_targets.h), then the JUMP_SLOT, which
 * must hold EffectTime itself (a word something else already replaced is refused, not chained over). A refused install leaves the game's calls as they
 * are. It registers in combat::InstallNormal()'s table first of the crash and timing guards (combat_install.cpp, kModules).
 *
 * Frequency (ADR 0038). EffectTime runs once per effect handle per query; how many per frame is not known (the module counts them:
 * `combat_fxtrail_stats`). Every call pays the thunk's counter and one relaxed atomic load of the scope word. A call in a combat map also reads the clock
 * (`LookIsFresh`), pays up to four process_vm_readv calls (the old plugin's cost, kept) and, when the effect has no instance, one counter and a log line at
 * 1, 2, 4, 8 ... ended trails. The host cost per call is in docs/quest/combat/fxtrail.md; not measured on a headset.
 *
 * Owner (ADR 0039). This file owns the value EffectTime answers for an effect with no instance, in a combat map.
 *
 * Origin. Written here on the reference build's `afk_fxtrail.cpp` (Ducky's echoquest-combat;
 * docs/audits/2026-11-05-q15t-origin-citations.md rows 63-68 and 212-213: afk_fxtrail.cpp:1,41,60-77 and the rule at :1-24). The word pins are an addition to it; the scope is the runtime's (combat_scope.h, #2246).
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_fxtrail_install","result":"installed|refused|off","reason":"ok|off_config|<token>","hook":<GotStatus>,
 *             "scope":"combat_map","consequence":".."}   Info for installed and off, Error for refused (reason scope_not_live; pinned_word_mismatch
 *             adds "pin" (the word's name), "vaddr", "found", "expected")
 *   ended    {"event":"combat_fxtrail_ended","n":<count>,"consequence":".."}  Info, at the 1st, 2nd, 4th, 8th ... ended trail
 *   stats    {"event":"combat_fxtrail_stats","calls":<n>,"in_combat":<n>,"ended":<n>,"final":0|1,"consequence":".."}
 *            Debug every kStatsEvery combat-map calls, Info once at the end of the process when the guard is installed
 *   legacy   {"event":"combat_fxtrail_legacy_switch","key":"combat.fxtrail","file":"fxtrail.off","consequence":".."}  Warning
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/fxtrail_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::fxtrail {

inline constexpr unsigned kStatsEvery = 16384;  // a Debug stats line every this many calls made in a combat map (a power of two)

// The two words the install pins: the slot and the function it must hold. The host test supplies a stand-in's address.
struct Pins {
  std::uint64_t slot, fn;
};
inline constexpr Pins kPins = {kEffectTimeSlotVaddr, kEffectTimeVaddr};

// Installs on the libr15 `lookup` finds. `on` is combat.fxtrail; Logs one combat_fxtrail_install line. A module that is off by its switch answers `off` (status "off_config"): nothing is wrong
// and nothing was done.
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept;

// The table's row: InstallAt on the loaded libr15 with the production pins and the data directory.
ModuleResult Install(bool on) noexcept;

struct Stats {
  std::uint64_t calls;     // every call of EffectTime that reached the handler (the thunk's counter)
  std::uint64_t inCombat;  // those made while a combat map was loaded
  std::uint64_t ended;     // trails ended: calls that answered FLT_MAX
};
Stats CurrentStats() noexcept;

// The table's end-of-process step: writes the final combat_fxtrail_stats line when the guard is installed, nothing otherwise.
void NoteEnd() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// How the install finds libr15 (production: the loaded image). nullptr restores production.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// Forgets the hook, clears the thunk and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::fxtrail
