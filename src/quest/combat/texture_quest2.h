/* Quest 2 eye buffer one step down in combat (#1737, docs/quest/combat/texture_quest2.md): combat's input to the texture-quality owner.
 *
 * What it does. On a Quest 2 whose player has not chosen a texture level, the eye buffer is Very Low (1152 x 1264 per eye, the Quest 2 profile's)
 * while a combat map, or the combat lobby with `combat.texture_quest2_lobby`, is loaded and settled, and the player's level everywhere else. The
 * decision and the Resize are the owner's (sentinel/texture_menu_hook.cpp, sentinel/texture_quality_rules.h); this module only says where the game
 * is (ADR 0039: the owner gains an input, and no second module touches the eye size).
 *
 * The look. A combat-tick callback (combat_tick.h), once every kLookEveryMs, reads the loaded levels (combat_level.h `Scan`, the two loaded tables)
 * and the count of CncaGame's init fibers (game + 0x1f08, combat_level_targets.h kFiberCount: the gamespaces still loading). It publishes the class
 * and whether any gamespace is loading: a count that is not 0, or that cannot be read, is loading (fails closed: the owner makes no look-driven
 * Resize while it reads so). The owner's poller and render thread read the published words (texture_quality_input.h); this module reads no
 * game memory on their threads.
 *
 * Why the guard is the load window (docs/quest/combat/texture_quest2.md "The guard"). A live Resize takes the game's upload lock at least nine times among the calls read (not all of CGDisplay::Resize is read) and,
 * holding it, waits for the upload count to reach 0; an upload taken and not yet submitted on another thread while it waits is the deadlock
 * (inferred, not observed). The owner therefore makes a look-driven Resize only while no gamespace has loaded for 10 s, and reads the upload count
 * and lock first as a narrowing, not as the guard.
 *
 * Switch and default. `combat.texture_quest2_step_down`, default FALSE (opt-in): the deadlock is unobserved, and a hang is worse than not stepping
 * down; the headset row decides flipping it. `combat.texture_quest2_lobby`, default true: the combat lobby is combat-only (combat_scope.h), and
 * with it off every match adds two look-driven Resizes around its load windows.
 *
 * Arena cost. With the key false nothing is registered: no tick callback, and the owner's per-frame apply reads one null pointer. With it true the
 * tick callback reads the clock once per frame in every level and scans every kLookEveryMs (one `Scan` and one 8-byte read), arena included, as
 * refresh_rate does; the owner's per-frame apply reads three atomics. Nothing is hooked.
 *
 * Lines (sentinel log):
 *   install {"event":"combat_texture_quest2_install","result":"installed|off|refused","reason":"ok|off_config|inactive|tick_not_installed|
 *            tick_table_full","lobby":<0|1>,"msg":".."}  Info, Error on a refusal
 *   change  {"event":"combat_texture_quest2_look","where":"none|combat_map|combat_lobby|echo_arena","loading":<0|1>,"fibers":<n|-1>,"looks":<n>}
 *            Info, once per change of the class or of loading (a load begins or ends), at most kChangeLines, then counted in `looks`
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/sentinel/texture_quality_rules.h"

namespace nevr_quest::combat::texture_quest2 {

inline constexpr std::int64_t kLookEveryMs = 500;
inline constexpr int kChangeLines = 64;

// The row's normal step: off with `on` false; refused when combat is not active or the tick cannot run the look; otherwise registers the tick
// callback and the owner's input.
ModuleResult Install(bool on, bool lobby) noexcept;

// The tick callback (combat_tick.h TickFn). Exposed for the host test.
void Tick(std::uintptr_t game) noexcept;

// The look the owner reads (texture_quality_input.h LookFn): the last published one, {kNone, loading, 0} before the first.
nevr_quest::texture_quality::rules::Look CurrentLook() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The monotonic clock in milliseconds (nullptr restores CLOCK_MONOTONIC).
void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept;
// Forgets the look, the schedule, the counters and the registration (the owner's input too).
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::texture_quest2
