/* The combat area's two install steps (#2171, docs/adr/0046 rule 6): where the folded combat modules go in, relative to the game's own
 * setup, and the one line each step writes.
 *
 * The moments. The early step is a constructor step of the sentinel's sequence (integration/ctor_sequence.h `install_combat_early`):
 * after Attach and the early plugin pass, before the clock hook and long before `android_main`. A module that must act before the game
 * reads its own configuration registers a function for it. The normal step is a post-load action (integration/post_load.h): it runs on the
 * first non-null `dlopen` the game makes after the sentinel's login and matchmaking installs and after the normal plugin pass has returned,
 * on the thread that ran it. The q15t plugin installs first on a slot both touch (ADR 0046 rule 7); a module that has moved in must not
 * install before the plugin's hooks are in.
 *
 * The gate. Both steps ask `combat::Active()` (combat_active.h) and install nothing when it is false: a plain release runs no module of
 * combat's, and a step that is reached anyway writes one `skipped` line and runs no module function. The sequence does not reach them in
 * that case (it skips `install_combat_early` and asks for no post-load action), so the line is the second line of defence, not the first.
 *
 * The table. Each phase runs one registration table, in the table's order: `Modules()` in combat_install.cpp. A module registers one
 * function per phase it has a step in, or null for none, at the position the q15t plugin installed it (plugin.cpp's order: decals after
 * impactsnd, wallfire after bvh, the net-tick modules after ec, and so on); the order is explicit, never the order of a directory or a
 * link. A function is `noexcept`, returns whether the module installed and a fixed status token, and writes the module's own lines
 * itself (`combat_<module>_*`).
 *
 * The line (sentinel log), one per phase per process (N17: each module's outcome by name, and one aggregate):
 *   {"event":"combat_install","phase":"early|normal","state":"ran","variant":"none|q15t|combat|perf-probe","active":1,
 *    "active_reason":"variant|forced|...","modules":<n>,"installed":<n>,"failed":<n>,"results":"<name>=<status> ...","results_truncated":0,"msg":"..."}
 * `active`, `results_truncated` and `force` are 1 or 0, as in the sentinel's `config_loaded` line.
 * Info when every module installed (and when none is registered), Warning when one did not. A process where combat does not run writes
 * `"state":"skipped","active":0` once, Info, whichever phase asks first. A normal step that never got its moment writes
 * `"state":"not_started","reason":"post_load_hook_unavailable|game_exited_first"` once, Warning, and never runs afterwards.
 *
 * The config line (`combat_config`, ADR 0046 rule 4): one Info line with each `combat:` key's value and where it came from, written by
 * the constructor's configuration step: `force`, and each module's switch (`heaps`, `memsys`, ...) as `<module>` and `<module>_source`
 * (`config` when the file gave it, `default` otherwise). The step also hands the configuration to SetConfig, which the registration table's
 * rows read for their module's switch.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>

#include "quest/sentinel/quest_config.h"

namespace nevr_quest::combat {

enum class Phase { kEarly, kNormal };
const char* PhaseName(Phase phase) noexcept;  // "early", "normal"

// What one module's install function answers: whether it installed, and a fixed token for the line (never a value).
struct ModuleResult {
  bool installed;
  const char* status;
};
using InstallFn = ModuleResult (*)() noexcept;

// One row of the registration table: the module's name (its `combat_<module>_*` events) and its step in each phase, null for none.
struct Module {
  const char* name;
  InstallFn early;
  InstallFn normal;
};

// What a phase did. `ran` is false when it did not run (combat is off in this process, or the phase already ran or was ended).
struct Outcome {
  bool ran = false;
  std::size_t modules = 0;    // the modules that have a step in this phase
  std::size_t installed = 0;
  std::size_t failed = 0;
};

// The two steps, over the shipped table. Each runs at most once per process.
Outcome InstallEarly() noexcept;
Outcome InstallNormal() noexcept;

// The phase over `table[0..count)`, in order: the rule the two steps share, with the table injected (the host test drives it with fakes).
Outcome RunPhase(Phase phase, const Module* table, std::size_t count) noexcept;

// Why a normal step never got its moment: the fixed tokens of the `reason` field (the plugin loader's tokens for the same two ends).
namespace reason {
inline constexpr const char* kPostLoadHookUnavailable = "post_load_hook_unavailable";  // the hook that makes the moment cannot be live
inline constexpr const char* kGameExitedFirst = "game_exited_first";                    // the game's android_main returned before it
}  // namespace reason

// Ends the normal step as `not_started` with `reason`: one record, and the step never runs afterwards. Writes nothing when combat is off
// in this process, when the normal step already ran, or when it was already ended.
void NoteNormalNotStarted(const char* reason) noexcept;

// Writes the `combat_config` line for `config`.
void LogConfig(const nevr_quest::CombatConfig& config) noexcept;

// Writes the `combat_config_table_steps` line for `config`: the four resource table-step switches, each with its value and source.
void LogTableStepConfig(const nevr_quest::CombatConfig& config) noexcept;

// The configuration the modules' switches are read from (their rows in the registration table): set once, by the constructor's
// configuration step, before either install step runs. Until it is set every switch has its default.
void SetConfig(const nevr_quest::CombatConfig& config) noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Forgets which phases ran or ended, so the next call runs and logs again, and the configuration SetConfig was given.
void ResetInstallForTest() noexcept;

}  // namespace nevr_quest::combat
