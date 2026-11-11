/* A CR15NetBulletCS::FireBullet call whose velocity FireBullet cannot normalise is not made (#1737 row 031, #1651;
 * docs/quest/combat/bullet_velocity_guard.md).
 *
 * What the game does. FireBullet normalises its velocity argument (x4, `const C3Vector&`) with no test: `fsqrt` at libr15 ELF 0x210ccc0 and
 * `fdiv` (1.0 / |v|) at 0x210ccc4 (bullet_velocity_guard_targets.h pins both). A zero vector divides by zero and stores a NaN direction in the
 * bullet's record; a non-finite component stores a non-finite one.
 *
 * What this does. A CallbackThunk on FireBullet's JUMP_SLOT (dronemuzzle_targets.h's pin, not repinned): when
 * `nevr_combat::bullet_velocity_guard::Skips(vel)` (runtime/combat/bullet_velocity_guard_rules.h: null, |v|^2 <= 1e-6, or |v|^2 not finite) it returns
 * 0 without calling FireBullet, counts the shot and writes a line; every other call goes to FireBullet with the same eight arguments and its
 * result is returned. dronemuzzle's drone-shot test uses the same rule (one definition).
 *
 * Order on the slot. dronemuzzle's assembly entry on the same slot must stay the newest hook there (got_hook.h, "Assembly entries"): a hook
 * installed after it is refused `slot_held_by_entry`. So this row sits immediately above dronemuzzle's in combat_install.cpp's table: the guard
 * installs first, the entry joins above it and calls it, and a shot the guard skips is skipped for dronemuzzle's continuation too.
 *
 * Arena (same APK). It acts in every level, on purpose: README rule 9's crash-only exception. A shot with no usable velocity divides by zero in
 * code arena and combat share, and every call that does not take that path reaches FireBullet unchanged, so an arena match sees the game's own
 * function. Cost per FireBullet call (once per shot, not per frame): one relaxed atomic increment, three multiplies, two adds and two compares.
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants (the normal step of combat_install.h). Switch: `combat.bullet_velocity_guard`, default
 * true (upstream's recipe ships its fireguard off; the doc says why this one is on).
 *
 * Owner (ADR 0039). This file owns whether a FireBullet call with no usable velocity reaches FireBullet. dronemuzzle owns the slot's newest hook.
 *
 * Origin. The fireguard switch of kotorvr's echoquest-combat, runtime/native/afkxr/afk_fx.cpp (its `HookFire` hook and "guards" block; the lines
 * are named in bullet_velocity_guard_targets.h), read as text and rebuilt on this tree's hook manager and rule.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_bullet_velocity_guard_install","result":"installed|off|refused","reason":"ok|off_config|<token>","slot":"0x..","msg":".."}
 *            Info for installed and off, Error for a refusal (module_not_loaded, no_build_id, build_id_mismatch, pinned_word_mismatch with "pin",
 *            "vaddr", "found", "expected"; or the manager's status by name: original_mismatch, slot_held_by_entry, ...)
 *   skip     {"event":"combat_bullet_velocity_guard_skip","n":<n>,"vx":"..","vy":"..","vz":"..","index":<handle & 0xffff>,"msg":".."}  Info, the first
 *            kSkipLines skips of the process ("vel":"null" in place of the three components for a null vector); then
 *   skips    {"event":"combat_bullet_velocity_guard_skips","n":<n>,"since_last":<n>,"msg":".."}  Info, one line saying later skips are counted
 *            (since_last 0), then the count at most every kSkipEveryMs while skips continue (nevr::BurstThenCount)
 *   total    {"event":"combat_bullet_velocity_guard_total","skipped":<n>,"calls":<n>,"msg":".."}  Info at the end of the process, installed only
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/combat/bullet_velocity_guard_targets.h"
#include "quest/combat/combat_install.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::bullet_velocity_guard {

inline constexpr unsigned kSkipLines = 10;              // skip lines written one by one per process
inline constexpr std::uint64_t kSkipEveryMs = 10000;    // then a counted line at most this often while skips continue

// FireBullet(CR15NetBulletCS* x0, SCompactPoolHandle x1, CComponentHandle<CR15NetGunCS> (by reference) x2, float s0, const CQuaternion& x3,
// const C3Vector& x4, unsigned w5, unsigned w6), as dronemuzzle.h declares it. FireBulletLocal (ELF 0x210bff8) passes its velocity argument as x4.
using FireSig = std::uint64_t(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float damage, const void* quat, const void* vel,
                              std::uint64_t remote, std::uint64_t flag);
struct FireTag {};
using FireThunk = sentinel::CallbackThunk<FireTag, FireSig>;

// The slot and the function it must hold (the store libr15's: dronemuzzle's pins). The host test supplies a stand-in's offset.
struct Pins {
  std::uint64_t slot, fn;
};
inline constexpr Pins kPins = {dronemuzzle::kPins.fireSlot, dronemuzzle::kPins.fireFn};

// The step (combat_install.cpp): `on` is `combat.bullet_velocity_guard`. Off answers {false, "off", off}.
ModuleResult Install(bool on) noexcept;
// The guard on the libr15 `lookup` finds, with `pins`: build ID, the three normalise words, then the slot.
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins) noexcept;

// The end-of-process step (the row's `end`): the total line, when the guard is installed.
void NoteEnd() noexcept;

std::uint64_t Skipped() noexcept;
std::uint64_t Calls() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------
// How Install finds libr15, and the pins it uses; nullptr restores production.
void SetInstallForTest(sentinel::ImageLookup lookup, const Pins* pins) noexcept;
// The clock the skip lines' budget reads (ms); nullptr restores CLOCK_MONOTONIC.
void SetClockForTest(std::uint64_t (*nowMs)()) noexcept;
// Forgets the hook, clears the thunk, the counters and the line budget.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::bullet_velocity_guard
