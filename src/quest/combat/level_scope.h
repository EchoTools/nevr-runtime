/* Which level class is loaded, published for the guards that act only while a combat level is (#2178, docs/adr/0046 rule 3 and the arena rule
 * in the README). Arena and combat run on the same APK, so `combat::Active()` does not say which one a player is in; a guard that changes
 * a value the game computes asks this instead of acting in an arena match. The decision is combat_level.h's (`Scan`: an Echo Arena level
 * that is loaded wins over any combat level still loaded during a transition); this file only looks at it on the game thread, on the
 * combat tick, and keeps the answer where a hook handler can read it with one atomic load.
 *
 * Why a published answer and not a read in the handler. Reading the gamespace tables costs a few process_vm_readv calls (combat_level.h);
 * the handlers this serves run once per effect or per network receive. The tick looks every kLookEverySeconds, on the game thread, and the
 * handlers read the result. The answer is therefore at most that old: a guard that scopes by it is one whose purpose does not need the
 * first half second of a level (a bullet trail that is never freed, a peer stamp in a session that has been running since the lobby).
 * A guard against a crash does not use it: the fault would not wait for the next look.
 *
 * Fail closed. Before the first look, and while the game is unreadable or the tick is not running, both answers are false: nothing is
 * scoped in. `Looks()` counts the looks so a log can tell a scope that said "no combat level" from one that never ran.
 *
 * Which APKs (ADR 0046 rule 5). A module asks `Start()` from its install function, which runs on the q15t and combat variants only, so a
 * plain release registers nothing here.
 *
 * Frequency (ADR 0038). `Tick` runs once per rendered frame: one clock read and one compare. Every kLookEverySeconds (in an arena match
 * too) it runs one `level::Scan`: two table reads and, per gamespace of the game, three more (process_vm_readv, never faulting); the
 * handlers' `InCombatMap` and `InCombatMapOrLobby` are one relaxed atomic load each. Host numbers are in the module docs that use it.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_quest::combat::scope {

inline constexpr double kLookEverySeconds = 0.5;

// A combat map is loaded and no Echo Arena level is.
bool InCombatMap() noexcept;
// A combat map or the combat lobby (`mpl_lobby_b_combat`) is loaded and no Echo Arena level is. The other lobby levels, which arena players
// use, are not combat's and never make this true.
bool InCombatMapOrLobby() noexcept;
// How many times the tick has looked at the game's levels (0: the scope has never run, so both answers are the fail-closed false).
std::uint32_t Looks() noexcept;

// Registers the tick callback that keeps the answers current; once per process, whichever module asks first. False when the tick table is
// full (the answers then stay false).
bool Start() noexcept;

// The tick callback (combat_tick.h). Exposed for the host test.
void Tick(std::uintptr_t game) noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The monotonic clock in seconds (nullptr restores CLOCK_MONOTONIC).
void SetClockForTest(double (*clock)() noexcept) noexcept;
// Forgets the answers, the look count, the schedule and the registration (the tick table is combat_tick's, reset there).
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::scope
