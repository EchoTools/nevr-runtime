#pragma once
// The bullet-velocity rule: whether a shot's velocity is one CR15NetBulletCS::FireBullet can normalise. FireBullet divides 1.0 by |v| with
// no zero or finiteness test (Quest: `fsqrt` at libr15 ELF 0x210ccc0, `fdiv` at 0x210ccc4, pinned in
// quest/combat/bullet_velocity_guard_targets.h; PC: echovr.exe 0x140ce2020, SQRTSS at 0x140ce2442, DIVSS at 0x140ce244e), so a zero vector
// gives an infinite reciprocal and a NaN direction, and a non-finite component gives a non-finite direction.
//
// This header is the one definition, for both builds: quest/combat/bullet_velocity_guard.cpp skips such a FireBullet call, and
// quest/combat/dronemuzzle.cpp leaves a drone shot's record alone on the same condition. PC calls nothing here yet: #2375 tracks the PC
// guard. It reads no memory and calls nothing of the game's.
//
// The rule: skip when the squared length is not above kMinSpeedSquared (|v| at most about 1e-3 m/s), when it is not finite (a NaN or infinite
// component, or a sum that overflows), and when there is no vector. The squared form is the comparison FireBullet's own sum gives, with no
// square root. `!(v2 > kMinSpeedSquared)` is true for a NaN sum; `!(v2 <= kMaxFinite)` is the isfinite test for the one value left, +inf
// (written without std::isfinite so the rule is constexpr).
//
// Origin: the fireguard switch of kotorvr's echoquest-combat, runtime/native/afkxr/afk_fx.cpp (the `HookFire` hook and its "guards" block),
// read as text; the threshold is the tree's own (dronemuzzle's drone-shot test), not upstream's.
#include <limits>

namespace nevr_combat::bullet_velocity_guard {

// |v|^2 at or below this skips: |v| at most about 1e-3 m/s (the float 1e-3f squared is just above it).
inline constexpr float kMinSpeedSquared = 1e-6f;
inline constexpr float kMaxFinite = std::numeric_limits<float>::max();

// True when FireBullet must not be called with `v` (three floats, x y z): null, |v|^2 <= kMinSpeedSquared, or |v|^2 not finite.
constexpr bool Skips(const float* v) noexcept {
  if (v == nullptr) return true;
  const float v2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  return !(v2 > kMinSpeedSquared) || !(v2 <= kMaxFinite);
}

}  // namespace nevr_combat::bullet_velocity_guard
