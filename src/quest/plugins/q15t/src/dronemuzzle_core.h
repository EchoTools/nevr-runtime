// Ported verbatim from the source build's dronemuzzle_core (drone fix F4;
// upstream "df" rule for rule) except the namespace (q15t::dm). Platform-free: MuzzleMove / FrameDt /
// ClampDt only; nothing here calls the game.
#pragma once
#include <math.h>

namespace q15t {
namespace dm {

constexpr float kMuzzle[3] = { 0.000024f, 0.075634f, 0.147283f };   // the gun's muzzle in the drone (x, y, z; m)
constexpr float kBackMax = 0.25f;                                   // m a bullet may start behind the muzzle

// a frame time in (1/240, 0.1) s feeds the running average 0.9 / 0.1; anything else leaves it
inline float FrameDt(float avg, float dt) { return dt > 1.0f / 240.0f && dt < 0.1f ? avg * 0.9f + dt * 0.1f : avg; }

// the frame-time clamp: 1/120 s to 1/30 s
inline float ClampDt(float dt) { return dt < 1.0f / 120.0f ? 1.0f / 120.0f : dt > 1.0f / 30.0f ? 1.0f / 30.0f : dt; }

// the bullet's spawn = root moved by kMuzzle in AttackTarget's basis (Z = aim, X = (z, 0, -x) / |..|,
// Y = Z x X) and back by one frame of travel, at most kBackMax. False: |v| < 1e-4, the pose stays as it is.
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

}  // namespace dm
}  // namespace q15t
