/* dronemuzzle (#2187, docs/adr/0046): a combat drone's bullets leave its gun (rows R49 CncaGameSpace::WorldTeleport and R50
 * CR15NetBulletCS::FireBullet; drone fix F4 + F6). docs/quest/combat/dronemuzzle.md has the disassembly reads; the rule is runtime/patch/dronemuzzle_rules.h (shared with PC).
 *
 * What it hooks. Two reviewed assembly entries (callback_thunk.h rule 7, dronemuzzle_entry.inc) on the two JUMP_SLOTs of dronemuzzle_targets.h,
 * through the hook manager (InstallAsmEntry). An entry needs the caller's return address as it is in x30, which a CallbackThunk's handler cannot
 * see. WorldTeleport has 25 callers and FireBullet 2; only the calls that return into FireBulletLocal (ELF 0x210c22c / 0x210c2a4) reach the
 * continuation, every other call goes straight to the next word with x0..x7 / v0..v7 / x30 / sp as the caller left them. In the continuation,
 * during a drone shot (dronekill::ShotVelocity non-null) the teleport sends a moved copy of the pose and FireBullet's record gets the shooter
 * fields of a drone's shot; anything else calls the next function unchanged.
 *
 * An entry acts on the game's own call site, so it stays the newest hook on its slot: the manager refuses a hook that asks to join above it
 * (`slot_held_by_entry`), and the entry installs above a handler already there and calls it (#2219). `dronemuzzle_test` runs both orders.
 *
 * Needs dronekill's shot. The step installs only when dronekill's FireBulletLocal hook is installed (dronekill::ShotActive); otherwise it refuses
 * with `needs_dronekill_shot` and installs nothing.
 *
 * Owner (ADR 0039). This file owns both slots. The module never had a switch file. The switch is
 * `combat.dronemuzzle`, default true: the reference build ships `dronemuzzle.on` and the combat plugin imported in #1591 built it on.
 *
 * Arena (same APK). The continuation acts only inside a drone shot, and dronekill makes no shot frame outside a combat map, so in an arena level
 * nothing is moved or written. A non-drone call (every WorldTeleport and FireBullet call an arena level makes) runs ten instructions of the
 * entry: `bti c`, three to load the call-site word with ldar, `cmp`, `b.eq` (not taken), three to load the next word with ldar, `br`
 * (docs/quest/combat/dronemuzzle.md, "Arena cost"). The tick only times frames.
 *
 * Which APKs (ADR 0046 rule 5). The combat variants only (the normal step of combat_install.h).
 *
 * Origin. The reference build's `runtime/native/afkxr/afk_dronemuzzle.cpp` (kotorvr's echoquest-combat; its thunks at lines 145-182), which ports
 * EchoCombat's `plugin/dronefix.cpp`: the muzzle transform df 916-941, FireBullet df 943-1019 (shooter velocity df 1012, tracers
 * df 1014), the frame time df 132-147 (df 136, df 138-147), the shot log cap df 56.
 *
 * Lines (sentinel log):
 *   hook     {"event":"combat_dronemuzzle_hook","hook":"<name>","status":"<GotStatus>"}  Info when installed, Error otherwise
 *   install  {"event":"combat_dronemuzzle_install","result":"installed|refused|off",..}
 *   shot     {"event":"combat_dronemuzzle_shot","shot":<n>,"remote":0|1,"root":"x y z","spawn":"x y z",..}  the first 40
 *   tracer   {"event":"combat_dronemuzzle_tracer","x":"..","y":"..","z":"..","index":<n>,..}  the first 3
 *   cap      {"event":"combat_dronemuzzle_cap","kind":"shot|tracer","cap":<n>}  once per kind
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/dronemuzzle_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::dronemuzzle {

struct Result {
  bool ok = false;  // both entries installed (all or none)
  const char* status = "not_tried";
  sentinel::GotStatus teleport = sentinel::GotStatus::kNotInstalled, fire = sentinel::GotStatus::kNotInstalled;
};

// The step (combat_install.cpp): `on` is `combat.dronemuzzle`. Fixed status tokens: "installed", "off", "needs_dronekill_shot", "already_ran",
// "module_not_loaded", "no_build_id", "build_id_mismatch", "tick_table_full", "hook_failed".
ModuleResult Install(bool on) noexcept;
// The two entries on the image `lookup` finds, with `pins`. A failed second install removes the first.
Result InstallAt(sentinel::ImageLookup lookup, const Pins& pins) noexcept;

// Once a frame on the game thread (combat_tick.h): the frame-time average the muzzle's "back by one frame" uses.
void Tick(std::uintptr_t game) noexcept;

struct Stats {
  std::uint32_t shots, tracers;
};
Stats CurrentStats() noexcept;

// The call-site and next words the entries read (absolute; 0 before the install), for the tests.
struct Words {
  std::uintptr_t teleportRet, teleportNext, fireRet, fireNext;
};
Words WordsForTest() noexcept;
void SetClockForTest(std::uint64_t (*now_ns)()) noexcept;
// The image lookup and the pins Install uses; nullptr restores production.
void SetInstallForTest(sentinel::ImageLookup lookup, const Pins* pins) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::dronemuzzle

// The two entries and their continuations (C names: the asm refers to them by symbol). Hidden.
extern "C" {
__attribute__((visibility("hidden"))) std::uint64_t nevr_combat_dm_tp_entry(void* gs, std::uint64_t actor, const std::uint8_t* pose, std::uint64_t reason);
__attribute__((visibility("hidden"))) std::uint64_t nevr_combat_dm_fire_entry(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float damage,
                                                                              const void* quat, const void* vel, std::uint64_t remote, std::uint64_t flag);
// F4: FireBulletLocal's teleport of a new bullet (x0..x3 as the caller passed them). A drone shot's pose goes on as a moved copy.
__attribute__((visibility("hidden"), noinline)) std::uint64_t nevr_combat_dm_teleport(void* gs, std::uint64_t actor, const std::uint8_t* pose,
                                                                                     std::uint64_t reason) noexcept;
// F6: FireBulletLocal's FireBullet (x0..x6 + s0). After it, a drone shot's record: shooter velocity 0, shooter position = its +0x134.
__attribute__((visibility("hidden"), noinline)) std::uint64_t nevr_combat_dm_fire(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float damage,
                                                                                 const void* quat, const void* vel, std::uint64_t remote,
                                                                                 std::uint64_t flag) noexcept;
}
