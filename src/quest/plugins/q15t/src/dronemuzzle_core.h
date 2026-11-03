// Drone bullet muzzle: where a drone's bullet starts, and the frame time its back-off uses. The rules are stated from the
// libr15.so disassembly (ReVault binary 576; VA = ELF vaddr + 0x100000). The game functions they act on, ELF vaddr [VA]:
//   CR15NetAutoTargetCS::AttackTarget     0x210b8c4 [0x220b8c4]: turns the drone's transform (rec+0x7c) to its aim and calls
//                                         FireBulletLocal with speed times the unit aim; the fire timer adds clock+0x3c each frame.
//   CR15NetAutoTargetCS::FireBulletLocal  0x210bff8 [0x220bff8]: passes the drone's transform as the new bullet's pose to
//                                         WorldTeleport (call at 0x210c228, return 0x210c22c), then FireBullet (call at 0x210c2a0).
//   CncaGameSpace::WorldTeleport          0x180a384 [0x190a384]: moves the bullet actor to the world pose (quaternion +0,
//                                         position +0x10, scale +0x1c).
//   CR15NetBulletCS::FireBullet           0x210c9c4 [0x220c9c4]: fills the bullet record. Its arg5 is 0 at every FireBulletLocal
//                                         call, so the shooter fields take the local player's velocity and position.
// Ducky's reference combat build (its drone fix F4) pointed at this defect and supplied the
// constants below, which the game does not yet confirm. Platform-free: MuzzleMove / FrameDt / ClampDt only; nothing here calls the game.
#pragma once
#include <math.h>

namespace q15t {
namespace dm {

// The drone gun's muzzle offset, in the gun's frame: x along X, y along Y, z along Z, metres. No game literal is known for it:
// a decimal search of the echovr project's decompilations (all 1,041 binaries) for 0.14728 and 0.07563 found nothing (the x value
// was not searched). These are the reference build's values; a headset run measures the bullet's start against them.
constexpr float kMuzzle[3] = { 0.000024f, 0.075634f, 0.147283f };
// Furthest the bullet may start behind the muzzle, metres: this module's cap on one frame of travel. The game has no back-off:
// FireBulletLocal passes the drone's transform to WorldTeleport unchanged (listing read in full, 0x220bff8 to 0x220c4a8).
constexpr float kBackMax = 0.25f;

// The frame time behind the back-off. A step counts only inside (1/240, 0.1) s and then weighs 0.1 against the running average's
// 0.9, so one hitch or one doubled tick does not move the spawn. DroneMuzzleTick feeds it from the monotonic clock: the module does
// not read the game's own per-frame delta (the float at clock+0x3c that AttackTarget adds to the fire timer).
inline float FrameDt(float avg, float dt) { return dt > 1.0f / 240.0f && dt < 0.1f ? avg * 0.9f + dt * 0.1f : avg; }

// The frame time is held to [1/120, 1/30] s, so the back-off is at least one frame at 120 fps and at most one at 30 fps.
inline float ClampDt(float dt) { return dt < 1.0f / 120.0f ? 1.0f / 120.0f : dt > 1.0f / 30.0f ? 1.0f / 30.0f : dt; }

// The bullet's spawn: the pose's position moves by kMuzzle in the gun's frame, then back along the aim by one frame of travel
// (at most kBackMax). Z is the aim, v / |v|: AttackTarget's speed times the unit vector of (target - origin), which FireBulletLocal
// receives as its direction. X is world up crossed with Z, normalised: (z, 0, -x) / |(z, 0, -x)|. Y = Z x X. AttackTarget writes the
// rows into the matrix it passes to the callee at 0x1005370 (instructions 0x220bb94 to 0x220bc08): (z, 0, -x) unscaled, Z x X scaled
// by 1/|(z, 0, -x)|, and Z. So Y = Z x X holds for the normalised X used here; whether that callee normalises the X row is not read.
// The quaternion and scale of the pose are kept. False: |v| < 1e-4, the pose stays as it is.
inline bool MuzzleMove(const float* root, const float* v, float frameDt, float* out) {
    const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len >= 1e-4f)) return false;
    const float z[3] = { v[0] / len, v[1] / len, v[2] / len };
    const float xl = sqrtf(z[2] * z[2] + z[0] * z[0]);
    const float x[3] = { xl > 1e-4f ? z[2] / xl : 1.0f, 0.0f, xl > 1e-4f ? -z[0] / xl : 0.0f };
    const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    const float dt = ClampDt(frameDt);
    const float back = len * dt > kBackMax ? kBackMax / len : dt;
    for (int i = 0; i < 3; ++i) {   // the offset is summed, then added to the root: this order fixes the float result the host test compares
        out[i] = root[i];
        out[i] += kMuzzle[0] * x[i] + kMuzzle[1] * y[i] + kMuzzle[2] * z[i] - v[i] * back;
    }
    return true;
}

}  // namespace dm
}  // namespace q15t
