// Drone bullet muzzle: where a drone's bullet starts, from the drone's transform and the shot velocity. Platform-free: nothing here
// calls the game.
//
// The game's code on this path (libr15.so, ReVault binary 576). Every address is a VA: libr15.so's ELF vaddr + 0x100000.
//   CR15NetAutoTargetCS::AttackTarget     VA 0x220b8c4. Builds the aim matrix at sp+0x28..0x48 from the unit aim u: row 0 is
//                                         (uz, 0, -ux) unscaled, row 1 is Z x X / |X| (the division at 0x220bbdc), row 2 is Z = u.
//                                         It calls NRadEngine::CQuaternion::FromRotationMatrix through its thunk (VA 0x1005370) at
//                                         0x220bc08; the thunk's indirect callee is not resolved to one target. The drone's rotation
//                                         is then set: immediately (0x220bc40) when the flag at [[this+0x130]+0x80]+0x368 is set, else
//                                         queued (0x220bcdc). Both paths reach the fire timer (0x220bc60..0x220bc78), which calls
//                                         FireBulletLocal (0x220bca8) once the timer reaches rec+0x6c.
//   CR15NetAutoTargetCS::FireBulletLocal  VA 0x220bff8. Takes the velocity in s0..s2 (stored at sp+0x1e0). Its pose is the drone's
//                                         transform, ComputeLocalToWorld(this+0x130, rec+0x7c), 32 bytes at sp+0xe0, passed unchanged
//                                         to WorldTeleport (call 0x220c228, return 0x220c22c). FireBullet (call 0x220c2a0, return
//                                         0x220c2a4) gets x3 = the pose, x4 = the velocity, x5 (`remote`) = 0 (mov w5, wzr at 0x220c270),
//                                         x6 = 0 (0x220c27c) and s0 = rec+0x70.
//   CR15NetGunCS::ComputeMuzzleXF         VA 0x22529d0: a gun's muzzle transform. Its callers are FireGunLocal (VA 0x2255454),
//                                         ExclusiveUpdate (VA 0x2252c94) and FUN_021554f4 (VA 0x22554f4), all through thunk_FUN_022529d0
//                                         (VA 0x10136a0). None of them is on the drone's chain: the call graph has Update (VA 0x220aeb0)
//                                         calling AttackTarget, and FireBulletLocal is called by AttackTarget and by FireBulletNetwork
//                                         (VA 0x220ac80). So ComputeMuzzleXF is not the drone's muzzle rule.
//
// The spawn rule (kMuzzle, in the aim basis, from the pose's position) and its three constants are Ducky's reference combat build's
// (its drone fix F4). FireBulletLocal adds no offset: it passes the pose it computed to WorldTeleport and FireBullet unchanged. The
// constants are not in the code read, and the data sections were not searched (no tool here reads data). They stay unconfirmed until a
// headset run measures the bullet's start.
#pragma once
#include <math.h>

namespace q15t {
namespace dm {

// The drone gun's muzzle offset, in the gun's frame: x along X, y along Y, z along Z, metres. Ducky's reference combat build's values
// (its drone fix F4). They are not found in the code read (see above); a headset run measures the bullet's start against them.
constexpr float kMuzzle[3] = { 0.000024f, 0.075634f, 0.147283f };

// The spawn: the pose's position plus kMuzzle in the aim basis. v is the shot velocity FireBulletLocal passes to FireBullet (x4): the
// speed times the unit aim. Z = v / |v|; X = (z, 0, -x) / |(z, 0, -x)|, which is AttackTarget's row 0 with its length divided out;
// Y = Z x X, the game's row 1 for that X. The basis is built from v and not from the pose's quaternion, because the drone's rotation
// write may be queued (0x220bcdc), and whether the queue drains before the pose is read (0x220c204) was not read. When the aim is
// vertical (xl <= 1e-4), X is (1, 0, 0); the game's value there was not read. False, with out left as it was, when |v| < 1e-4 or v is
// NaN. The back-off an earlier version applied (at most 0.25 m along the aim, with a frame-time term) is dropped: FireBulletLocal passes
// the pose unchanged, the tree records no reason for the back-off, and it moved the spawn behind the offset the reference gives.
inline bool MuzzleMove(const float* root, const float* v, float* out) {
    const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len >= 1e-4f)) return false;
    const float z[3] = { v[0] / len, v[1] / len, v[2] / len };
    const float xl = sqrtf(z[2] * z[2] + z[0] * z[0]);
    const float x[3] = { xl > 1e-4f ? z[2] / xl : 1.0f, 0.0f, xl > 1e-4f ? -z[0] / xl : 0.0f };
    const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    for (int i = 0; i < 3; ++i) {   // the offset is summed, then added to the root: this order fixes the float result the host test compares
        out[i] = root[i];
        out[i] += kMuzzle[0] * x[i] + kMuzzle[1] * y[i] + kMuzzle[2] * z[i];
    }
    return true;
}

}  // namespace dm
}  // namespace q15t
