/* dronekill (#2187, docs/adr/0046): a combat drone that dies goes off, a drone that has locked on can be grabbed until it fires, its gun, satellite
 * and port stay one model, it chases its target, and its shot hands dronemuzzle the shot's velocity. Rows R45, R47, R48 and the Actor slot Enter's
 * capture needs; docs/quest/combat/dronekill.md has the disassembly reads.
 *
 * What it hooks (dronekill_targets.h, four CallbackThunks on JUMP_SLOTs of libr15, through the hook manager):
 *   SComponentNodeUtils::Actor            Enter's lookup: while a drone's Enter runs on this thread, a copy of the game space and SActor it found
 *   CEnableComponentNode::Enter           the drone's enable/disable nodes: the groups (kill), the touch interact (lock-on), the gun model (gun)
 *   CR15NetAutoTargetCS::FireBulletLocal  a drone's shot: lock-on, and the shot velocity dronemuzzle reads (ShotVelocity)
 *   CR15NetAutoTargetCS::AttackTarget     a drone's attack: the chase velocity, set at the tick
 * What it calls (never writes): FindComponentSystemMT (const), the systems' vtable slots Component / DisableComponent / EnableComponent (only after
 * the read shapes of dronekill_core.h match), CTransformCS::ComputeLocalToWorldTransfQ, CPhysicsCS::PrimaryBody / SetWorldVelocity,
 * CPhTarget::ResolveBody, CR15NetPhysicsCS::IsLocallyControlled. Game memory is read through combat/safe_read.h.
 *
 * The explosion (#1737, docs/quest/combat/drone_explosion.md; `explosion`, the key `combat.dronekill_explosion`, default true). A dead drone
 * explodes on this client: a 'K' queues its explosion; its own end (the script turns kNcaPhysics off, then its damageable) queues it too. The
 * explosion runs at the tick (the game thread, outside any script node): the drone library's two entries, whose addresses its prelude stores
 * on the EC page (ec_core.h kDroneSpawn, kDroneSoundFn), each checked to be that library's entry at its built offset, are called with the
 * context kept at the drone's release, its state word at +0x18 zeroed round each and put back after both (PC dronefix.cpp SpawnDeathFx).
 * With the hold off, the first-system off is the game's own Enter, run untouched; once it has run, its actor names the drone's group (the
 * one live group of that actor whose end is not seen), whose expiry is then open, and that drone's damageable off, while the group is alive
 * and a context was kept, queues the explosion with nothing held (sound_delay_ms 0). This is PC's order (dronefix.cpp Hooked_EnableEnter
 * runs Real_EnableEnter for that node too) without PC's deferred disable.
 *
 * The hold (`hold`, the key `combat.dronekill_hold`, default FALSE; it acts only with the explosion on). It skips the real Enter of the
 * first-system off and turns the same component off kFxSoundHoldMs after the explosion. That is safe only if CEnableComponentNode::Enter,
 * for a disable, has no effect but the component's disable (vtable +0x210, now or through its game space's deferred queue, owner +0x368).
 * That is NOT established: a ReVault read of Enter (ELF 0x273b700, VA 0x283b700) suggests it, the proven reconstruction does not hold
 * that function, and its reconstruction is requested (the reconstruct pipeline's NEEDED list). The hold stays off until that reconstruction
 * shows the one effect; turning it on by default is one constant (quest_config.h `dronekillHold`). With it on, a hold is taken only when
 * exactly one drone group can still run its end (Enter has not looked its actor up yet; a shot-down drone counts until its own end has been
 * seen, however long that takes), that group is alive, its actor has the component, the system's DisableComponent is the read shape and its game
 * space's invoke-immediate word reads; otherwise the real Enter runs at once (the game's behaviour, then the expiry opens by actor as above).
 * A held component goes off through the same stage-1 path as a kill's (OffOne): kFxSoundHoldMs after the explosion, at once when the drone
 * was already dead, had no context or the queue was full, or kFxSoundHoldMs after the hold when no damageable off came. When OffOne cannot
 * turn it off (the component was looked up again and differs, the game space defers the disable for kDueMs, or the off list is full) the
 * component STAYS ON and nothing turns it off: one Warning `combat_dronekill_hold` step `stays_on` says so. When the scope ends (kLeaveAsks no-level
 * answers in a row) with holds open, one Warning (step `dropped`, `count`) says dronekill forgot them and they stay on.
 *
 * The kill feed. The EC client hands each first-time 'K' datagram (a drone's health ran out on the server) to OnKilled: the install registers it
 * with ec::OnDroneKilled (ec.h). Without an EC server no 'K' arrives and `kill` never fires; lock-on, gun, chase and shot do not need it.
 *
 * Owner (ADR 0039). This file owns the four slots. The q15t plugin no longer carries dronekill. It never had a switch file, so there is no legacy
 * file to warn about; the switch is `combat.dronekill`, default true: the reference build ships `dronekill2.on` (all five words) and the q15t plugin built it on (`Q15T_DRONEKILL`).
 *
 * Arena (same APK). Arena and combat run on the same APK, and Actor and Enter are script-node functions every level's scripts call, so the module
 * acts only while a combat map is loaded (combat_level.h `Classify`, asked by the module's own tick every kScopeFrames frames and kept in one
 * atomic, InCombatScope; the scope ends after kLeaveAsks no-level answers in a row, so a failed read does not end it). Outside a combat map every hook calls the game's function and returns: Enter and FireBulletLocal before anything else,
 * AttackTarget after its call, Actor after its call when no drone Enter is in flight; OnKilled drops the 'K'; the tick clears the tables when the
 * scope ends. An arena level writes only the one `combat_dronekill_scope` line per change of scope; the process writes the final
 * `combat_dronekill_stats` record at its end whenever the module is installed, with zero counts if no combat map was seen.
 * The scope asks `combat_level` directly; moving it to `combat/combat_scope.h`'s `InCombatMap` is tracked in #2382. `InCombatScope()` is the one place that changes.
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants only (the normal step of combat_install.h).
 *
 * Frequency (ADR 0038). Enter and Actor run once per execution of a script node of that kind, in any level; FireBulletLocal once per drone shot;
 * AttackTarget once per frame per drone that has a target. Not measured on a headset: the `combat_dronekill_scope` line carries each hook's call
 * count over the scope that ended. The out-of-scope path of each is counted in docs/quest/combat/dronekill.md ("Arena cost").
 *
 * Origin. The reference build's `runtime/native/afkxr/afk_dronekill.cpp` (kotorvr's echoquest-combat), which ports EchoCombat `c14c3e0a`
 * `plugin/dronefix.cpp`: LockedOn df 805-827, ChaseTarget df 1059-1095 (no target df 1069-1076, the chase table df 1093).
 *
 * Lines (sentinel log):
 *   hook     {"event":"combat_dronekill_hook","hook":"<name>","status":"<GotStatus>"}  Info when installed, Error otherwise
 *   install  {"event":"combat_dronekill_install","result":"installed|refused|off","reason":"..",..}  one per step
 *   scope    {"event":"combat_dronekill_scope","in_combat":0|1,"level":"0x..",..}  one per change of scope
 *   scope_wait {"event":"combat_dronekill_scope_wait","no_level_asks":n,"needed":3,..}  Debug, per no-level answer that did not yet end the scope
 *   stats    {"event":"combat_dronekill_stats","enter_drone":n,..,"expired":n,"final":0|1}  Debug every kStatsEvery drone Enters, Info at the process end
 *   expired  {"event":"combat_dronekill_expired","index":n,"group":n,"tid":n,..}  Info per drone end that found a live group, the first 16 of a run
 *   log_cap  {"event":"combat_dronekill_log_cap","lines":"expired","cap":16,..}  Warning, once, at the drone end past the cap
 *   explosion          {"event":"combat_dronekill_explosion","index":n,"group":n,"reason":"killed|expired","sound_delay_ms":2500|0,"entries":2,..}
 *                      Info per explosion run, the first 64 of a run (then one log_cap line; the stats line keeps counting)
 *   explosion_skipped  {"event":"combat_dronekill_explosion_skipped","index":n,"reason":"queue_full|no_entry|foreign_entry|no_context",..}
 *                      Warning per explosion not run, the first 16
 *   hold               {"event":"combat_dronekill_hold","index":n,"group":n,"step":"held|released|off_next_tick|let_go|not_held|end_open|end_not_open|stays_on|dropped",..}
 *                      Debug, the first 64; stays_on: Warning, the first 16 (a held component that could not be turned off stays on);
 *                      dropped: Warning, the held components the end of the scope left on
 * and the acting lines (`combat_dronekill_component`, `_release`, `_off`, `_k`, `_touch`, `_gun`, `_chase`, `_note`), each capped per kind with a
 * `combat_dronekill_cap` line when the cap is reached.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/dronekill_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::dronekill {

// The five rules (the reference build's TEST words; all on, as its QA file).
struct Words {
  bool kill = true, lockon = true, gun = true, chase = true, shot = true;
  bool explosion = true;  // `combat.dronekill_explosion` (#1737): a dead drone's explosion ('K' and expiry)
  bool hold = false;      // `combat.dronekill_hold` (#1737): its first-system off held back for it; off until Enter's one effect is established
};

struct DkC3 {  // C3Vector: an HFA, passed in s0-s2
  float x, y, z;
};
struct DkTransfQ {  // CTransfQ, 0x20 bytes (returned through x8)
  float q[4];
  float p[3];
  std::uint32_t w;
};
struct DkPhTarget {  // CPhTarget, 0x18 bytes (returned through x8)
  std::uint64_t tag;
  std::uint32_t body, zone, gen, pad;
};

// The libr15 functions the module calls. Resolved by ResolveFns (every export must sit at its pinned vaddr), faked in tests.
struct Fns {
  void* (*find)(const void* space, std::uint64_t system_sym) = nullptr;  // FindComponentSystemMT const
  DkTransfQ (*l2w)(const void* transforms, std::uint32_t handle) = nullptr;
  DkPhTarget (*primary)(const void* physics, std::uint32_t handle) = nullptr;
  void* (*resolve)(const DkPhTarget* t) = nullptr;
  void (*set_vel)(void* physics, std::uint32_t handle, DkC3 v) = nullptr;
  std::uint32_t (*local)(const void* net_physics, std::uint32_t handle) = nullptr;
};

// Resolves the six functions by name from the loaded libr15.so and requires each at base + its pinned vaddr. False (and one Error line naming
// the function) when one is missing or elsewhere.
bool ResolveFns(std::uintptr_t base, Fns* out) noexcept;

struct Result {
  bool ok = false;  // every wanted hook installed (all or none)
  const char* status = "not_tried";
  sentinel::GotStatus actor = sentinel::GotStatus::kNotInstalled, enter = sentinel::GotStatus::kNotInstalled,
                      fire = sentinel::GotStatus::kNotInstalled, attack = sentinel::GotStatus::kNotInstalled;
  bool comparatorOk = false;  // the comparator's JUMP_SLOT holds its function: Component() look-ups are allowed
  bool needsTick = false;     // kill / lockon / chase are on: the tick has due work to run
};

// The step (combat_install.cpp): `on` is `combat.dronekill`. Off: one line, nothing installed. On: the loaded libr15 (its build ID), the six
// functions, the tick (the scope), the four hooks, the kill feed. Fixed status tokens: "installed", "off", "already_ran", "module_not_loaded",
// "no_build_id", "build_id_mismatch", "engine_export_missing", "tick_table_full", "no_find", "slot_not_pinned_function", "no_thread_key",
// "hook_failed".
ModuleResult Install(bool on, const Words& words = Words{}) noexcept;

// The hooks on the image `lookup` finds, with `pins` (kPins in production; a host test points them at its own functions) and `fns`. All hooks or
// none: every wanted slot is read and must hold its pinned function before the first install; a failed install removes the earlier ones.
// FireBulletLocal is hooked when lockon or shot is on, AttackTarget when chase is on. One line per hook and one summary line.
Result InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Words& words, const Fns& fns) noexcept;

// The game's frame tick (combat_tick.h): every kScopeFrames frames it asks combat_level whether a combat map is loaded (the scope); in scope it
// runs the due work (disables after a 'K', the touch interact on and off, the chase velocities); one atomic load while nothing is due.
inline constexpr std::uint32_t kScopeFrames = 32;
// Leaving the scope takes this many no-level answers in a row (one ask every kScopeFrames frames): the level read cannot tell an unreadable game from
// one with no combat map, so one answer does not end the scope. Entering takes one.
inline constexpr std::uint32_t kLeaveAsks = 3;
void Tick(std::uintptr_t game) noexcept;

// Whether a combat map is loaded, as the tick last saw it (false before the first tick: fail closed). The one place the module asks.
bool InCombatScope() noexcept;

// The 'K' feed: the damageable index a server "drone killed" datagram carries (16 bits are compared). Safe from any thread; table work under the
// module's lock, one capped line. Dropped out of scope.
void OnKilled(std::uint32_t index) noexcept;

// The shot accessor: the velocity of the shot FireBulletLocal is making on this thread, null outside the real call, out of scope, or when the
// FireBulletLocal hook is not installed. *remote = 1 for a remote shot. The pointer is valid until the real call returns (it points into the
// handler's frame).
const float* ShotVelocity(int* remote) noexcept;
bool ShotActive() noexcept;  // the FireBulletLocal hook is installed (the accessor can be non-null)

// combat_dronekill_stats (#2359): Debug every kStatsEvery drone Enters, Info once at the process end (NoteEnd). Its fields are these counters.
inline constexpr unsigned kStatsEvery = 16384;  // a Debug stats line every this many drone Enters (a power of two)
struct Stats {
  std::uint32_t enter_drone, enter_other, fires, attacks, ticks, due_runs;
  std::uint32_t capturing;  // drone Enters in flight now (0 between calls)
  std::uint32_t expired;    // drone ends the D11 rule saw: a drone's damageable went off and found its group
  std::uint32_t explosions; // explosions run (both entries called)
  std::uint32_t held;       // first-system offs held back for an explosion
};
Stats CurrentStats() noexcept;
// The process end (combat_install's table step): the final stats record, when the module is installed.
void NoteEnd() noexcept;

// ---- test seams ----
// The check ResolveFns makes once the names are looked up: addrs[i] must be base + the pinned vaddr of function i (order: find, l2w, primary,
// resolve, set_vel, local). The host has no libr15.so to look the names up in; the test feeds the addresses.
bool ResolveFnsFromAddressesForTest(std::uintptr_t base, const void* const addrs[6], Fns* out) noexcept;
// How the module calls game code through vtable slots; the host test replaces them (a slot's words are aarch64 data on the host).
struct Calls {
  std::uint64_t (*component)(std::uintptr_t fn, const void* system, std::uint64_t actor, std::uint64_t sym);
  void (*enable)(std::uintptr_t fn, void* system, std::uint64_t handle, std::uint32_t category);
  void (*disable)(std::uintptr_t fn, void* system, std::uint64_t handle, std::uint32_t category);
};
void SetCallsForTest(const Calls* calls) noexcept;  // null = the real calls
void SetClockForTest(std::uint64_t (*now_ms)()) noexcept;
// The image lookup, the pins and the functions Install uses; nullptr restores production.
void SetInstallForTest(sentinel::ImageLookup lookup, const Pins* pins, const Fns* fns) noexcept;
// Makes ShotVelocity answer `v3` (3 floats) on this thread, as inside a FireBulletLocal call; null clears it. Which thread answers is per thread,
// but the values are one frame for the process.
void SetShotForTest(const float* v3, int remote) noexcept;
void SetShotActiveForTest(bool active) noexcept;  // what ShotActive answers
void SetScopeForTest(bool inCombat) noexcept;     // what InCombatScope answers until the next refresh
// What the explosion asks of an entry word (dladdr in production): the test answers with its own library name and base.
struct EntryInfo {
  const char* fname;
  std::uintptr_t fbase;
};
void SetEntryInfoForTest(bool (*info)(std::uintptr_t fn, EntryInfo* out)) noexcept;  // null = dladdr
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::dronekill
