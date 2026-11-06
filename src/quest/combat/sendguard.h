/* A script send to a game space that is not loaded is dropped, not faulted (#2179, #1603; the q15t plugin's sendguard, moved here).
 *
 * What the game does (store libr15; addresses are the ELF vaddr, sendguard_targets.h; the disassembly lists each 0x100000 higher).
 * NRadEngine::CSendEventNode::Enter (0x2725a80, 476 bytes) sends a script event to the game space its node names. As C++ (x0 the node's
 * execution context, x1 the node data):
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
 * which asks again (a lookup with no side effect; the engine calls it from many per-frame paths). A null link on the chain, a null
 * context or a null node is left to the stock function, which reads the same words.
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
 * plugin installed it (after tablestep, before levelname).
 *
 * Frequency (ADR 0038). Enter runs once per script send-event node executed, from the game's script update (the callers were read in
 * ReVault; calls per frame were not measured). The wrapper's cost is four loads and one GameSpaceMT call, which is one lookup; the cost
 * on a headset was not measured.
 *
 * Owner (ADR 0039). This file owns whether a send to an unloaded game space reaches the stock function. The arguments and the result of
 * a send to a loaded space are not changed.
 *
 * Origin. Written for this project from the game's own code (the disassembly cited above); the reference build and EchoCombat have no
 * counterpart, and docs/audits/2026-11-05-q15t-origin-citations.md lists no origin token for it. It came with the q15t plugin that
 * ef714347 imported.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_sendguard_install","result":"installed|refused","hook":<GotStatus>,"consequence":".."}
 *            or {"event":"combat_sendguard_install","result":"refused","why":"module_not_loaded|no_build_id|build_id_mismatch|
 *            pin_mismatch","pin":"<name>" (pin_mismatch only),"consequence":".."}, Error
 *            or {"event":"combat_sendguard_install","result":"off","reason":"off_config","consequence":".."}, Info
 *   dropped  {"event":"combat_sendguard_dropped","n":<count>,"level_key":"0x..","event_key":"0x..","actor_key":"0x..","consequence":".."}
 *            Info, the first kDropLines of the process; every drop is counted (CurrentStats).
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

inline constexpr unsigned kDropLines = 8;  // `dropped` lines written per process, then counted only

// The plugin's data directory, where its switch file `sendguard.off` was read from.
inline constexpr const char* kLegacySwitchDir = "/sdcard/Android/data/com.readyatdawn.r15/files/q15t";

// Installs on the libr15 `lookup` finds. `enabled` is combat.sendguard; `legacySwitchDir` is where a `sendguard.off` of the old plugin
// would sit (nullptr: not looked for). Logs one combat_sendguard_install line.
// installed true: the wrapper is in, or the module is off by its switch (status "off_config": nothing is wrong, and nothing was done).
ModuleResult InstallAt(sentinel::ImageLookup lookup, bool enabled, const char* legacySwitchDir) noexcept;

// The table's row: InstallAt on the loaded libr15, with combat.sendguard from the `combat_config` the sequence resolved.
ModuleResult Install() noexcept;

struct Stats {
  unsigned dropped;  // sends dropped because the named game space is not loaded
  unsigned passed;   // sends that reached the stock function
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Replaces the GameSpaceMT lookup the next InstallAt publishes (a host image holds the stub's pinned words and cannot hold callable
// host code there). nullptr restores base + the stub's vaddr.
void SetLookupForTest(GameSpaceMtSig* lookup) noexcept;
// Forgets the hook, clears the thunk, the counters and the log budget.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::sendguard
