/* Whether a combat level is loaded, as a value a hot path can read for free (#2186, docs/adr/0046): `InCombat()` is one relaxed load.
 *
 * Why. Arena and combat run on the same APK, so `combat::Active()` (per APK) does not say which one a player is in, and a hook that is
 * installed on a slot arena's scripts also call must tell the two apart on every call. Asking the game (combat_level.h `Scan` reads
 * two tables of gamespaces with process_vm_readv) once per call would put a system call on arena's path; this module asks it a few
 * times a second, from the game-thread tick, and keeps the answer.
 *
 * The answer. True while a combat map or the combat lobby is loaded and no Echo Arena level is (combat_level.h `Classify` with the lobby
 * rule on: an Echo Arena level wins over any other level still loaded during a transition). False in every Echo Arena level, in the
 * lobbies arena players use and everywhere else, and false until the first look: it fails closed, so a module that asks it does nothing
 * for the first quarter second of a process and whenever the tick is not running.
 *
 * Who uses it. The wrist HUD (hudmeter.h) and the shield colour (shieldgamma.h). A module calls `Enroll()` from its install function,
 * before it hooks anything; the first call registers the refresh with the combat tick (combat_tick.h), later calls find it there.
 *
 * Frequency (ADR 0038). `Refresh` runs once per rendered frame on the game thread, after the game's frame update: it reads the monotonic
 * clock (a vDSO call, no system call) and compares it with the next look. Every kLookEveryMs it looks: two tables, up to
 * 64 gamespaces each, a handful of process_vm_readv calls in practice (the arena path's measured cost is in the pull request that added
 * this file). `InCombat()` is one relaxed atomic load, whichever thread asks.
 *
 * Lines (sentinel log):
 *   enroll  {"event":"combat_scope_enroll","result":"registered|already|refused","callbacks":<n>,"msg":".."}  once per outcome change
 *   change  {"event":"combat_scope_change","from":<0|1>,"to":<0|1>,"level":"0x%016llx","looks":<n>,"msg":".."}  Info, one per change of the answer
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::combat::scope {

inline constexpr std::int64_t kLookEveryMs = 250;

// Registers the refresh with the combat tick (once). False when the tick's callback table is full: the answer then stays false for good.
bool Enroll() noexcept;

// True while a combat map or the combat lobby is loaded (see above). One relaxed load.
bool InCombat() noexcept;

// The tick callback (combat_tick.h `TickFn`): looks at the game when kLookEveryMs have passed since the last look. Exposed for the host test.
void Refresh(std::uintptr_t game) noexcept;

// How many times the game's tables were read (a look is one `Scan`).
std::uint32_t Looks() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The monotonic clock in milliseconds (nullptr restores CLOCK_MONOTONIC).
void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept;
// Forgets the answer, the schedule, the counters and the registration with the tick.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::scope
