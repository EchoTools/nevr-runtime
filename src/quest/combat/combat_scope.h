/* Whether a combat level is loaded, as a value a hot path reads for free (#2186, docs/adr/0046): `InCombatOrLobby()` and `InCombatMap()` are a load
 * and a test on the path of an arena match, and one more clock read on the path of a combat one.
 *
 * Why. Arena and combat run on the same APK, so `combat::Active()` (per APK) does not say which one a player is in, and a hook that is
 * installed on a slot arena's scripts also call must tell the two apart on every call. Asking the game (combat_level.h `Scan` reads two tables
 * of gamespaces with process_vm_readv) once per call would put a system call on arena's path; this module asks it a few times a second, from
 * the game-thread tick, and keeps the answer.
 *
 * The answer. The three classes one `level::Scan` reports, as bits of one atomic word: a combat map, the combat lobby (`mpl_lobby_b_combat`) and an
 * Echo Arena level. The two questions the modules ask are derived from them in the header, so the rule is in one place:
 *   InCombatMap()       a combat map is loaded and no Echo Arena level is;
 *   InCombatOrLobby()   a combat map or the combat lobby is loaded and no Echo Arena level is.
 * An Echo Arena level wins over any other level still loaded during a transition. The lobbies arena players use are no class. Both answers are
 * false in an Echo Arena level, in those lobbies and everywhere else.
 *
 * Fails closed, in three ways. (1) The word starts at zero, so nothing is combat until the first look. (2) A level the game cannot be read for is no
 * level. (3) An answer is only as good as its look: when the combat tick stops calling `Refresh` (the object it is called with fails the vptr check,
 * or the game thread stops calling the game's frame update), a true answer older than kStaleAfterMs is false. The word itself freezes, since nothing is
 * left to change it; the staleness test is made when a caller reads a true answer, so the path of an arena match (a word with no combat bit) never
 * reads the clock.
 *
 * Who owns the refresh. The combat install table has a `scope` row after the `tick` row (combat_install.cpp): `InstallNormal()` registers `Refresh`
 * with the tick (combat_tick.h) once, in the combat variants only. A module that reads the answer does not enrol it; it asks `Live()` from its install
 * function and refuses (`scope_not_live`) when the answer cannot change: the table row did not register, or the tick itself is not installed.
 *
 * Frequency (ADR 0038). `Refresh` runs once per rendered frame on the game thread, after the game's frame update: it reads the monotonic clock (a vDSO
 * call, no system call) and compares it with the next look. Every kLookEveryMs it looks: two tables, up to 64 gamespaces each, a handful of
 * process_vm_readv calls in practice. A read of the answer is one relaxed atomic load; a true one also reads the clock.
 *
 * The arm64 object of a module that reads it (hudmeter.h, shieldgamma.h) compiles the arena path to a load, a test, a branch and a tail call.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_scope_install","result":"registered|refused","reason":"ok|tick_not_installed|tick_table_full","callbacks":<n>,"msg":".."}
 *   change   {"event":"combat_scope_change","from":<0|1>,"to":<0|1>,"map":<0|1>,"level":"0x%016llx","looks":<n>,"msg":".."}  Info, one per change of
 *            either answer (`from` and `to` are InCombatOrLobby, `map` is InCombatMap after the change); none when an arena match starts
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "quest/combat/combat_install.h"

namespace nevr_quest::combat::scope {

inline constexpr std::int64_t kLookEveryMs = 250;
inline constexpr std::int64_t kStaleAfterMs = 1000;

inline constexpr std::uint32_t kBitCombatMap = 1U << 0;
inline constexpr std::uint32_t kBitCombatLobby = 1U << 1;
inline constexpr std::uint32_t kBitEchoArena = 1U << 2;

namespace detail {
// The classes of the last look. Written by the tick's callback, read by any thread.
inline std::atomic<std::uint32_t> g_bits{0};
// The monotonic time of the last look in ms. Read only by `LookIsFresh`, when a combat bit is set.
inline std::atomic<std::int64_t> g_lookedAtMs{0};
}  // namespace detail

// Whether the last look is recent enough to be believed (kStaleAfterMs). Out of line: it reads the clock.
bool LookIsFresh() noexcept;

// The last look's answers, not yet checked for staleness: one relaxed load and a test, and no call. A hook whose arena path must stay that
// short (hudmeter.h, shieldgamma.h) tests these first and checks `LookIsFresh()` in the function it hands a combat call to.
inline bool SeenCombatMap() noexcept {
  return (detail::g_bits.load(std::memory_order_relaxed) & (kBitCombatMap | kBitEchoArena)) == kBitCombatMap;
}
inline bool SeenCombatOrLobby() noexcept {
  const std::uint32_t bits = detail::g_bits.load(std::memory_order_relaxed);
  return (bits & kBitEchoArena) == 0 && (bits & (kBitCombatMap | kBitCombatLobby)) != 0;
}

// True while a combat map is loaded and no Echo Arena level is (and the answer is fresh).
inline bool InCombatMap() noexcept { return SeenCombatMap() && LookIsFresh(); }

// True while a combat map or the combat lobby is loaded and no Echo Arena level is (and the answer is fresh).
inline bool InCombatOrLobby() noexcept { return SeenCombatOrLobby() && LookIsFresh(); }

// The module's normal step (the `scope` row): registers the refresh with the combat tick, once. Not installed (and nothing registered) when the
// tick's own install was refused: the callback would never run.
ModuleResult InstallNormal() noexcept;

// Whether the answer can change: the row registered the refresh and the tick is installed. A module that reads the answer asks it before it hooks
// anything.
bool Live() noexcept;

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
