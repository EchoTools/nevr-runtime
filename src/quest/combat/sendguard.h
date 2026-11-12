/* A script send to a game space that is not loaded is dropped, not faulted (#2179, #1603; the q15t plugin's sendguard, moved here).
 *
 * What the game does (store libr15; addresses are the ELF vaddr, sendguard_targets.h; the disassembly lists each 0x100000 higher).
 * NRadEngine::CSendEventNode::Enter (0x2725a80, 476 bytes) is the script node that sends a component event to the game space it names
 * (CncaGameSpace::SendComponentEvent, 0x2725b90): the script-event path inside the game, not the network send path. As C++ (x0 the
 * node's execution context, x1 the node data):
 *   void Enter(Ctx* ctx, SSendEventNodeData* d) {
 *     space = ctx->frame->owner->spaceHolder->space;                            // 0x2725a90..0x2725ab0, the last `ldr x19,[x9,#0x80]`
 *     if (d->level != -1) space = CncaGameSpace::GameSpaceMT(space, d->level);  // 0x2725aa4 `cmn x1,#1`; 0x2725abc `bl` the stub
 *     if (!SComponentNodeUtils::Actor(ctx, <the first space>, d->actor, ...)) { ScriptActorError(...); return; }   // 0x2725af4
 *     event = { d->event, space->[0x58], ... };                                 // 0x2725b78 `ldr x9,[x19,#0x58]`: faults when space == 0
 *     CncaGameSpace::SendComponentEvent(space, &event);                         // 0x2725b90
 *   }
 * The Actor lookup is given the first space, never the looked-up one, so it passes while the named level is absent and the function goes
 * on to read the null space. The engine's other callers of GameSpaceMT test its answer (PlayAnimation 0x146c5c4, CanPlayAnimation
 * 0x146e2b4, UpdateBindings 0x1490b6c); this function does not. The case seen: a Quest without the combat content pack, once the menu
 * gate lets the menu take the non-Quest branch: SIGSEGV at libr15 offset 0x2725b78 with x19 = 0.
 *
 * What this does. The wrapper repeats the stock function's own first reads (the chain to the current space and the level key), asks
 * GameSpaceMT the same question, and, when the answer is null, returns without sending. Every other send goes to the stock function,
 * which asks again (a lookup that is idempotent: it may sort the game's space table once after a change, and the stock call does the
 * same; the engine calls it from many per-frame paths). A null link on the chain, a null context or a null node is left to the stock
 * function, which reads the same words.
 *
 * What else is dropped. The stock function faults only when the level is absent and the actor lookup (0x2725af4) succeeds; when the
 * actor does not resolve either, it reports ScriptActorError (0x2725b48) and returns. The guard drops that send too, and the game's
 * report is not made. This is logged, not passed through: each of the first kDropLines drops says so in its consequence, so a send that
 * would have reported a missing actor is named in the log. (Letting the case through would mean evaluating the actor lookup, which is
 * game code with effects of its own.)
 *
 * The one slot. Enter is called through one JUMP_SLOT (0x36f7928). The q15t plugin also swapped a second "slot", 0x356a6b0, which is
 * an unwind record inside `.eh_frame` and no pointer (sendguard_targets.h); it was refused at every launch with a Warning, and is not
 * named here.
 *
 * Which APKs (docs/adr/0046 rule 5). It runs where combat::Active() is true (combat_active.h): the q15t and combat variants. Its switch
 * is `combat.sendguard` in config.yaml (default true). The plugin's `sendguard.off` file is no longer read; one found in the files
 * folder's q15t directory is a Warning that names the key.
 *
 * Install. The build ID, then the code pins (sendguard_targets.h), then the JUMP_SLOT, checked against base + Enter's address (a word
 * that holds anything else, such as a word another runtime replaced, is refused and logged by GotHook with its chain). A pin that
 * differs, or a refused slot, leaves the game's calls as they are. It registers in combat::InstallNormal()'s table, at the position the
 * plugin installed it: in the normal phase after the headset gates and the table steps, before the pfx and pfxtev decisions.
 *
 * Frequency (ADR 0038). Enter runs once per script send-event node executed, from the game's script update. Calls per frame were not
 * measured; the module counts them (passed and dropped) and writes the counts, so a headset run gives the rate (combat_sendguard_stats,
 * below). The cost, counted from the arm64 code: every call pays two atomic increments (the thunk's call counter and `passed`) and two
 * indirect branches; a call that names a level also pays five dependent loads and one GameSpaceMT lookup. No time was measured.
 * It acts in every level on purpose: the fault it exists for happened in the lobby (a menu script's event to a combat sub-level a Quest
 * without the combat content pack never loaded), so a scope by level would leave it out.
 *
 * Owner (ADR 0039). This file owns whether a send to an unloaded game space reaches the stock function. The arguments and the result of
 * a send to a loaded space are not changed.
 *
 * Origin. Written for this project (#1604) from the game's own code (the disassembly cited above); the reference build and EchoCombat
 * have no counterpart, and docs/audits/2026-11-05-q15t-origin-citations.md lists no origin token for it.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_sendguard_install","result":"installed|refused","hook":<GotStatus>,"consequence":".."}
 *            or {"event":"combat_sendguard_install","result":"refused","why":"module_not_loaded|no_build_id|build_id_mismatch|
 *            pin_mismatch","pin":"<name>" (pin_mismatch only),"consequence":".."}, Error
 *            or {"event":"combat_sendguard_install","result":"off","reason":"off_config","consequence":".."}, Info
 *   dropped  {"event":"combat_sendguard_dropped","n":<count>,"level_key":"0x..","event_key":"0x..","actor_key":"0x..","consequence":".."}
 *            Info, the first kDropLines of the process; every drop is counted (CurrentStats).
 *   stats    {"event":"combat_sendguard_stats","passed":<n>,"dropped":<n>,"final":0|1,"consequence":".."}: Info once at the end of the process
 *            (final 1, written by NoteEnd when the wrapper is installed), and Debug every kStatsEvery passes and every kStatsDropEvery drops
 *            (final 0), so a process that is killed still leaves the rate and the drops after the eighth leave a trace.
 *   legacy   {"event":"combat_sendguard_legacy_switch","key":"combat.sendguard","file":"sendguard.off","consequence":".."}, Warning,
 *            when the plugin's switch file is found; the file changes nothing.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/sendguard_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::sendguard {

inline constexpr unsigned kDropLines = 8;        // `dropped` lines written per process, then counted only
inline constexpr unsigned kStatsEvery = 16384;   // a Debug stats line every this many passes (a power of two)
inline constexpr unsigned kStatsDropEvery = 1024;  // and every this many drops after the first kDropLines (a power of two)

// Installs on the libr15 `lookup` finds. `enabled` is combat.sendguard; `legacySwitchDir` is where a `sendguard.off` of the old plugin
// would sit (nullptr: not looked for). Logs one combat_sendguard_install line.
// installed true: the wrapper is in. A module that is off by its switch answers `off` (status "off_config"): nothing is wrong and nothing
// was done, and the phase line counts it apart from the installed ones.
ModuleResult InstallAt(sentinel::ImageLookup lookup, bool enabled, const char* legacySwitchDir) noexcept;

// The table's row: InstallAt on the loaded libr15, with `enabled` the value of combat.sendguard (the registration table reads it from the
// configuration the constructor's step handed to SetConfig).
ModuleResult Install(bool enabled) noexcept;

struct Stats {
  unsigned dropped;  // sends dropped because the named game space is not loaded
  unsigned passed;   // sends that reached the stock function
};
Stats CurrentStats() noexcept;

// The table's end-of-process step: writes the final combat_sendguard_stats line when the wrapper is installed, nothing otherwise.
void NoteEnd() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Replaces the GameSpaceMT lookup the next InstallAt publishes (a host image holds the stub's pinned words and cannot hold callable
// host code there). nullptr restores base + the stub's vaddr.
void SetLookupForTest(GameSpaceMtSig* lookup) noexcept;
// Forgets the hook, clears the thunk, the counters and the log budget.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::sendguard
