/* The frozen-frame exit (#2377, #1737 row G6A-045; docs/quest/combat/frozen_frame_exit.md): a combat game whose frames stop for
 * `combat.frozen_frame_exit_s` seconds (default 30) while the headset is in VR is ended, so the player lands in Home instead of a frozen view.
 *
 * What it reads. Two things, and it hooks nothing:
 *   - each frame submit: the render-configuration hook (sentinel/render_config_hook.cpp) owns the `vrapi_SubmitFrame2` slot and calls `NoteSubmit()`
 *     before the original (ADR 0039: this module is an input of that owner, not a second hook on the slot). A submit that blocks inside VrApi so
 *     counts as frozen;
 *   - the game's in-VR word, libr15 ELF 0x37a33f0 (frozen_frame_exit_targets.h): the `ovrMobile*` EnterVrMode returned, zero when the game left VR
 *     (LeaveVrMode, a session that is not active, ShutdownGraphics). Read with one aligned 8-byte atomic load after the build ID and the pinned words
 *     were checked at install. Out of VR the game makes no submit, so no limit runs.
 *
 * The watchdog. A thread (`nevr-frozenframe`) wakes once a second on a condition variable with a CLOCK_MONOTONIC deadline (no sleep). On each wake:
 * out of VR, or no combat class in the last look (`scope::SeenCombatOrLobby()`, read without its freshness test, because a freeze also stops the
 * tick that refreshes it), it disarms (the stamp back to 0, so the next submit arms again). Otherwise, when `rules::Expired` says the time since the
 * last submit is past the limit, it reads the word and the stamp once more, and when they still say so it:
 *   1. writes the Warning `combat_frozen_frame_exit` (the sentinel log writes each record as it is made, so nothing waits for a flush);
 *   2. finishes the game's activity (`quit::FinishActivity`, the end the menu's QUIT takes), when there is an activity to finish;
 *   3. waits kGraceMs for the game to end itself that way (a frozen game may not: the finish is handled on the game's main thread);
 *   4. writes the Warning `combat_frozen_frame_exit_end` and ends the process with `process_end::EndWithExitCode(kFailureExitCode, "frozen_frame")`,
 *      which writes the `android_main_returned` record and runs the before-exit step (each combat module's end line) first.
 * It fires at most once per process.
 *
 * Scope (D8 (a), recorded on #2377). The combat APK only: the install step is a row of the combat install table, which runs only where
 * `combat::Active()` is true, and `Watching()` is the combat scope bit (a combat map or the combat lobby, no Echo Arena level) and the module armed.
 * Arena on the same APK: `NoteSubmit` is one relaxed load and a branch per submit (the scope word has no combat bit), no clock read, no write; the
 * watchdog disarms. A release APK: the row never runs, so `NoteSubmit` reads an empty scope word and returns.
 *
 * Switch. `combat.frozen_frame_exit` (default true; upstream's recipe ships its hangwd off, #1737 G6A-045 asks for it on) and
 * `combat.frozen_frame_exit_s` (10..900, default 30; anything else: the default, with the configuration step's Warning).
 *
 * Origin. The hangwd watchdog of kotorvr/echoquest-combat, runtime/native/afkxr/afk_capture.cpp (namespace hangwd: Beat in the SubmitFrame2 wrapper,
 * Enter and Leave in the VrApi mode wrappers, a watcher that ends the process), read as text and rebuilt on this tree's owners: the submit is the
 * render-configuration hook's, VR mode is the game's own word (no VrApi mode hook), the end is the QUIT finish and process_end's exit.
 *
 * Owner (ADR 0039). This file owns ending a game whose frames stopped in VR. It changes no argument and no result of any game function.
 *
 * Frequency (ADR 0038). `NoteSubmit` runs once per rendered frame: outside a combat level one relaxed load and a branch; inside one, a clock read
 * (vDSO), one atomic exchange and one atomic increment. The watchdog runs once a second: one atomic load of the word and of the scope word, and a
 * clock read only while armed in a combat level.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_frozen_frame_exit_install","result":"installed|off|refused","status":"ok|off_config|scope_not_live|submit_slot_not_held|
 *             word_unreadable|pinned_word_mismatch|thread_not_started","limit_s":<n>,"limit_source":"config|default","scope":"combat_or_lobby","msg":".."}
 *             Info for installed and off, Error for a refusal (word_unreadable adds "why": module_not_loaded|no_build_id|build_id_mismatch|outside_image;
 *             pinned_word_mismatch adds "pin", "vaddr", "found", "expected"; thread_not_started adds "errno")
 *   fired    {"event":"combat_frozen_frame_exit","since_submit_ms":<n>,"limit_s":<n>,"scope_bits":<n>,"submits":<n>,"armed_ms":<n>,
 *             "action":"finish_activity|no_activity","msg":".."}  Warning, once
 *   end      {"event":"combat_frozen_frame_exit_end","grace_ms":<n>,"action":"end_process","msg":".."}  Warning, once, right before the exit
 *   counts   {"event":"combat_frozen_frame_exit_counts","arms":<n>,"submits":<n>,"fired":<0|1>,"joined":<0|1>,"msg":".."}  Info at the end of the
 *             process (the row's end), installed only; joined 0 when the watchdog is the one ending the process
 * A submit writes no line (callback_thunk.h rule 1: NoteSubmit runs in the handler's live frame).
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/combat_scope.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::frozen_frame_exit {

inline constexpr unsigned kPollMs = 1000;   // the watchdog wakes this often
inline constexpr unsigned kGraceMs = 5000;  // after the finish, how long the game has to end itself

namespace detail {
// Installed and the watchdog runs. Written by the install and the end, read by every submit.
inline std::atomic<bool> g_armed{false};
}  // namespace detail

// Whether a submit is watched now: the last look saw a combat map or the combat lobby (and no Echo Arena level), and the module is armed. The
// scope is tested first (README rule 9): one relaxed load and a test on arena's path.
inline bool Watching() noexcept { return scope::SeenCombatOrLobby() && detail::g_armed.load(std::memory_order_relaxed); }

// The stamp of a watched submit (out of line: it reads the clock). Called only through NoteSubmit.
void Stamp() noexcept;

// Called by the render-configuration hook's vrapi_SubmitFrame2 handler before the original, once per frame. Writes no line, allocates nothing.
inline void NoteSubmit() noexcept {
  if (!Watching()) return;
  Stamp();
}

// The step (combat_install.cpp): `on` is `combat.frozen_frame_exit`, `limitS` is `combat.frozen_frame_exit_s` and `limitFromFile` whether the file
// gave it. Off answers {false, "off", off}.
ModuleResult Install(bool on, int limitS, bool limitFromFile) noexcept;
// The same on the libr15 `lookup` finds (the host test supplies an image built in memory).
ModuleResult InstallAt(bool on, int limitS, bool limitFromFile, sentinel::ImageLookup lookup) noexcept;

// The end-of-process step (the row's `end`): stops and joins the watchdog unless the watchdog is ending the process, then the counts line. Installed
// only.
void NoteEnd() noexcept;

// One wake of the watchdog: what the thread does each kPollMs. Exposed for the host test.
void Tick() noexcept;

// The last watched submit in CLOCK_MONOTONIC ms, 0 when not armed. And the counters.
std::int64_t LastSubmitMs() noexcept;
std::uint64_t Submits() noexcept;
std::uint64_t Arms() noexcept;
bool Fired() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// What the watchdog calls to end the game; production: quit::CanFinishActivity, quit::FinishActivity, a kGraceMs wait on the watchdog's condition
// variable, process_end::EndWithExitCode. Null fields keep production.
struct Seams {
  bool (*canFinish)() noexcept = nullptr;
  bool (*finish)() noexcept = nullptr;
  void (*grace)(unsigned ms) noexcept = nullptr;
  void (*end)(int exitCode, const char* reason) noexcept = nullptr;
};
void SetSeamsForTest(const Seams& seams) noexcept;
// The monotonic clock in ms (nullptr restores CLOCK_MONOTONIC), and the watchdog's period (0 restores kPollMs).
void SetClockForTest(std::int64_t (*nowMs)() noexcept) noexcept;
void SetPollMsForTest(unsigned ms) noexcept;
// Arms the observer without an install (no thread, no word): a submit in scope stamps.
void ArmForTest() noexcept;
// Whether the watchdog thread runs.
bool ThreadRunningForTest() noexcept;
// Stops the thread, forgets the install, the stamp, the counters and the seams.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::frozen_frame_exit
