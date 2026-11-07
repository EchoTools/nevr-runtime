/* The combat area's game-thread tick (#2172, docs/adr/0046): one thunk on `CncaGame::ExclusiveUpdateGame` (combat_tick_targets.h), which
 * calls the game's function, checks that the object is the CR15Game, and runs the callbacks the combat modules registered, once per
 * frame, on the game thread, in registration order.
 *
 * Owner (ADR 0039). This file owns the combat modules' per-frame callback. A module registers with `Add`, from its install function; it
 * may do so before or after the thunk is installed.
 *
 * KNOWN-WRONG #2168: the q15t plugin still installs its own wrapper on the same slot for the modules that have not moved (its last
 * ticked module moves in F20). The plugin installs first, at the normal plugin pass, and this module installs after it (combat_install.h:
 * the normal step runs after the pass), so the slot holds the plugin's wrapper and this thunk joins the manager's chain above it through
 * the first-handler rule: the thunk calls the plugin's wrapper, which calls the game's function and its own callbacks, and then runs the
 * combat callbacks. The reverse order is what the plugin's own install refuses (it expects the game's function in the slot,
 * `unexpected_original`), which the runner's order never produces; `combat_tick_test` runs both orders on a fixture slot.
 *
 * Which APKs (ADR 0046 rule 5). Installed by the normal step of the q15t and combat variants only.
 *
 * Frequency (ADR 0038). Once per rendered frame: the handler does a counter increment, a compare with the object that already passed the vptr check
 * (the read is made again when the pointer changes and every 256 frames) and a walk of the callbacks, and writes nothing to the log except one Error the first time the object is not the CR15Game.
 *
 * Origin. The game-thread tick of the q15t plugin (`game_tick.cpp`, the reference build's `gunconnect` tick), reconstructed here; the
 * 16-callback table and the vptr check are its.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_tick_install","result":"installed|refused","status":"<GotStatus>","callbacks":<n>,"msg":".."}
 *   object   {"event":"combat_tick_object","readable":<0|1>,"msg":".."}   Error, once, when the object's vptr is not the CR15Game's
 * and the manager's own `got_hook` line for the slot, which names the chain.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::tick {

inline constexpr std::size_t kMaxCallbacks = 16;

using TickFn = void (*)(std::uintptr_t game) noexcept;

// Registers a callback. False when the table is full or `fn` is null. Safe at any time; a callback added after the install runs from the
// next frame.
bool Add(TickFn fn) noexcept;
std::size_t Count() noexcept;

// The module's normal step: installs the thunk on the loaded libr15 and writes the install line.
ModuleResult InstallNormal() noexcept;
// The install on the libr15 the lookup finds (the host test supplies an image built in memory).
bool InstallAt(sentinel::ImageLookup lookup) noexcept;

// The CR15Game the tick last verified (its first word is the pinned vptr), or 0 before the first verified frame. A module that acts on the game
// outside a frame (a resource constructor on a loading thread) reads the game's levels through it. Never cleared once set: a re-check of the
// same object does not make it vanish for a loading thread; combat_level.h's reads fail closed on an object that is gone.
std::uintptr_t Game() noexcept;

// How many frames the handler has run.
std::uint32_t Calls() noexcept;
// How many times the handler read the object's vptr: once for the object that passed, again when the pointer changes and every 256 frames, and
// once per frame for an object that did not.
std::uint32_t VptrReads() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Forgets the hook, the thunk's state, the callbacks and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::tick
