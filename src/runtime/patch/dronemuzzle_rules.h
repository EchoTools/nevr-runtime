#pragma once
// Drone bullet muzzle: where a drone's bullet starts, from the drone's transform and the shot velocity (#2337). This header is the whole
// rule, for both builds: the muzzle offset, the back-off cap, the running frame time and its clamp, and the spawn. It reads no memory and
// calls nothing of the game's, so it needs no Env. The hooks that feed it are runtime/combat/dronefix.cpp on PC (TickFrame, MuzzleTransform)
// and quest/combat/dronemuzzle.cpp on Quest (the frame tick, the teleport continuation); each keeps its own clock, its own read of the pose
// and its own log lines. The PC GTest (runtime/tests/test_dronemuzzle_rules.cpp) and the Quest host test (quest/tests/dronemuzzle_test.cpp)
// both compile this one definition.
//
// Origin. EchoCombat's `plugin/dronefix.cpp` (he_is_the_cat, heisthecat31/EchoCombat): MUZZLE, BACK_MAX, the frame average and the
// transform. PC ports it at `c3ff4b54` (runtime/combat/dronefix.cpp), the Quest at `c14c3e0a` through the reference build's
// `runtime/native/afkxr/afk_dronemuzzle_core.h` (kotorvr's echoquest-combat; MuzzleMove, ClampDt and FrameDt are its lines), rule for rule:
// MUZZLE df 51, BACK_MAX df 52, the frame average df 143-144, the frame time df 928-929, the transform df 916-934, `p[i] += ...` df 933-934.
// The two ports have the same constants and the same arithmetic. They differed in one guard: PC skipped the move only when `len < 1e-4f`,
// which is false for a NaN length, so a NaN velocity moved the pose by NaN. The guard below, `!(len >= 1e-4f)`, leaves the pose alone
// for a NaN velocity too, on both builds (#2337: it guards against bad input; moving a pose by NaN is never gameplay).
//
// The game's code on this path, PC (echovr.exe; offsets from the image base, runtime/combat/dronefix.cpp):
//   CR15NetAutoTargetCS::FireBulletLocal  +0xce2550 (cs, handle, velocity, remote). Its call at +0xce27e3 to CTransformCS::get (+0x3c4470)
//                                         reads the bullet's spawn pose, which is the drone's root; dronefix.cpp redirects that one call
//                                         to MuzzleTransform, which applies MuzzleMove to the pose's position.
//   CR15NetFollowPlayerCS::Update         +0xd65cb0, once a frame: dronefix.cpp's TickFrame takes the frame interval there (QPC) and feeds
//                                         FrameDt.
//
// The game's code on this path, Quest (libr15.so, ReVault binary 576). Every address is a VA: libr15.so's ELF vaddr + 0x100000.
//   CR15NetAutoTargetCS::AttackTarget     VA 0x220b8c4. Builds the aim matrix at sp+0x28..0x48 from the unit aim: row 0 is (uz, 0, -ux)
//                                         unscaled, row 1 is Z x X / |X| (the division at 0x220bbdc), row 2 is Z. It calls FromRotationMatrix
//                                         through thunk 0x1005370 (call at 0x220bc08). ReVault names that thunk thunk_FUN_0140ad54; the
//                                         target was not resolved here. The drone's rotation is set at once (0x220bc40) when the flag at
//                                         [[this+0x130]+0x80]+0x368 is set, and queued otherwise (0x220bcdc).
//                                         Fire timer: the frame delta is the float at +0x3c of the clock object, read at 0x220bc60
//                                         (`ldr s0, [x0, #0x3c]`). It is added to the record's timer (+0x78) and compared with the fire rate
//                                         (+0x6c) at 0x220bc74. When the timer is reached it is cleared and FireBulletLocal is called (0x220bca8).
//                                         Speed: the record's speed (+0x74) multiplies the aim at 0x220bc88..0x220bca0, so the velocity is
//                                         speed times the unit aim. A negative speed reverses the velocity.
//   The drone's weapon record is 0x90 bytes: an ushort index from the table at [this+0xc8], the base at [this+0xf8]. The three setters write
//                                         the fields this module reads: SetFireRate (VA 0x220cde4) writes +0x6c, SetBulletDamage (VA 0x220cdbc)
//                                         writes +0x70, SetBulletSpeed (VA 0x220cd94) writes +0x74 (each has `mov w10, #0x90` and `madd`).
//   CR15NetAutoTargetCS::FireBulletLocal  VA 0x220bff8. Takes the velocity in s0..s2, stored at sp+0x1e0 and passed as x4 (0x220c268). The
//                                         drone's transform is read by the call at 0x220c208 (`bl 0x0103d910`, with x0 = this+0x130 and w1 = the
//                                         32-bit field at rec+0x7c), result at sp+0x108. WorldTeleport's pose argument (sp+0xe0) is a copy of
//                                         sp+0x108 (0x220c20c..0x220c224), and FireBullet's x3 is sp+0x108 too. The 32-byte pose passed to WorldTeleport is at sp+0xe0 (call 0x220c228, return
//                                         0x220c22c). FireBullet is called through PLT stub 0x1038c20 (call 0x220c2a0, return 0x220c2a4) with x3 =
//                                         sp+0x108 (read as a rotation at 0x220ca20), x4 = the velocity, x5 = 0 (`mov w5, wzr` at 0x220c270),
//                                         x6 = 0 (0x220c27c) and s0 = rec+0x70 (0x220c29c).
//   CR15NetBulletCS::FireBullet           VA 0x220c9c4. Tests x5 with cbz at 0x220ca44, and w22 (copied from w5 at 0x220ca10) with cbz at 0x220cacc.
//                                         w5 == 0 takes the path that reads this+0x1a0 and this+0x1a8 (0x220ca5c, 0x220cadc). That path is the
//                                         local player's, as those reads suggest; no symbol confirms it.
//                                         +0x134 is the bullet's spawn position: FireBullet stores the position of the pose that 0x0103d910
//                                         returns for the record's fields +0x30 and +0x38 (the call at 0x220cb34, the stores at 0x220cb40..0x220cb4c).
//                                         Where those fields are set was not traced, so it is not shown that they hold the pose WorldTeleport set.
//                                         FireBullet also writes rec+0xd0 = -1.0 (`mov w8, #0xbf800000` at 0x220ccb0, `str w8, [x24, #0xd0]` at
//                                         0x220cce8): the value UpdateMovement tests for a bullet's first step (see the clock note below).
//   CR15NetGunCS::ComputeMuzzleXF         VA 0x22529d0. Called at two sites, 0x2252f64 and 0x2255560 (xrefs of thunk 0x10136a0). Neither is on the
//                                         drone's chain: Update (VA 0x220aeb0) calls AttackTarget, and FireBulletLocal is called by AttackTarget and
//                                         by FireBulletNetwork (VA 0x220ac80). So it is not this rule.
//
// Not shown by the code read: the three constants below, the back-off cap, and which gun the bullet's pose comes from. The drone record
// carries a CComponentHandle<CR15NetGunCS> copied at 0x220c250..0x220c298, so the drone gun's own transform may be the real muzzle. That is a
// lead, not a finding.
//
// The spawn rule (Ducky's reference combat build's, its drone fix F4). With v the shot velocity (FireBulletLocal's velocity):
//   Z = v / |v|
//   X = (z2, 0, -z0) / |(z2, 0, -z0)|: AttackTarget's row 0 with its length divided out; (1, 0, 0) when that length is <= 1e-4
//   Y = Z x X: the game's row 1 for that X
//   spawn = root + kMuzzle[0] X + kMuzzle[1] Y + kMuzzle[2] Z - min(|v| dt, kBackMax) Z
// root is the pose's position. dt is the running average of host frame intervals (FrameDt and ClampDt below). The basis is built from v, not from the
// pose's quaternion, because the rotation write may be queued (0x220bcdc) and whether the queue drains before the pose is read (0x220c208) was
// not read.
//
// The Quest game reads its frame delta from the game space's clock: [[space+0x10]+0x3c], with space = [this+0x80] (AttackTarget 0x220bc44..0x220bc48,
// the float read at 0x220bc60). It calls GlobalClock() only when [space+0x10] is 0 (UpdateMovement 0x22242cc..0x22242e0). That call is the
// thunk at 0xfd4860, a GOT jump to 0x1907720, which loads `ldr x0, [0x386cb30]`. The time scale is read through one GOT word (VA 0x3812618,
// loaded at 0x22242f8; the float it points to is read at 0x2224318), and the step is the delta times the scale (0x2224414).
// The Quest module keeps main's clock on purpose and does not read the game's clock. It could read the delta from the `gs` that
// nevr_combat_dm_teleport already receives: FireBulletLocal passes [this+0x80] to WorldTeleport (x0 at 0x220c21c; the value comes through the
// thunk at 0xfe2f50, which jumps to GetST 0x18ff2fc, a bare ret), so no new address is needed. Switching to the game's clock is #2036's
// decision. dt here is a running average of host frame intervals (FrameDt, weights 0.9 and 0.1; 1/90 s until the second tick;
// CLOCK_MONOTONIC in dronemuzzle.cpp, QueryPerformanceCounter in dronefix.cpp), clamped by ClampDt. kBackMax is the reference build's cap;
// the code read does not show it.
#include <math.h>

namespace nevr_dronemuzzle {

// The drone gun's muzzle offset, in the aim basis built from v: x along X, y along Y, z along Z, metres. Ducky's reference combat build's
// values (its drone fix F4; tools/fix_drone_model.py's MUZZLE on PC). The code read does not contain them (see above); a headset run
// measures the bullet's start against them.
constexpr float kMuzzle[3] = { 0.000024f, 0.075634f, 0.147283f };
constexpr float kBackMax = 0.25f;   // m a bullet may start behind the muzzle: the reference build's cap, not read from the game

// a frame time in (1/240, 0.1) s feeds the running average 0.9 / 0.1; anything else leaves it
inline float FrameDt(float avg, float dt) { return dt > 1.0f / 240.0f && dt < 0.1f ? avg * 0.9f + dt * 0.1f : avg; }

// the frame-time clamp: 1/120 s to 1/30 s
inline float ClampDt(float dt) { return dt < 1.0f / 120.0f ? 1.0f / 120.0f : dt > 1.0f / 30.0f ? 1.0f / 30.0f : dt; }

// The spawn: the pose's position moved by kMuzzle in the aim basis (Z = aim, X = (z, 0, -x) / |..|, Y = Z x X), then back along the
// aim by min(|v| dt, kBackMax). dt is ClampDt(frameDt). False, with out left as it was, when |v| < 1e-4 or v is NaN.
inline bool MuzzleMove(const float* root, const float* v, float frameDt, float* out) {
    const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len >= 1e-4f)) return false;
    const float z[3] = { v[0] / len, v[1] / len, v[2] / len };
    const float xl = sqrtf(z[2] * z[2] + z[0] * z[0]);
    const float x[3] = { xl > 1e-4f ? z[2] / xl : 1.0f, 0.0f, xl > 1e-4f ? -z[0] / xl : 0.0f };
    const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    const float dt = ClampDt(frameDt);
    const float back = len * dt > kBackMax ? kBackMax / len : dt;
    for (int i = 0; i < 3; ++i) {   // `p[i] += ...`: the offset summed first, then added (same float result)
        out[i] = root[i];
        out[i] += kMuzzle[0] * x[i] + kMuzzle[1] * y[i] + kMuzzle[2] * z[i] - v[i] * back;
    }
    return true;
}

}  // namespace nevr_dronemuzzle
