// NRadEngine::CGRenderConfig::SetManualResolutionScale(float)
// Quest libr15.so VA 0x198d290; ReVault binary 576.
// ReVault raw parent 100258546, reconstruction node 100600228; grade H: all nine instructions establish the clamp order, constants and global store.
// @0x198d290 — NRadEngine::CGRenderConfig::SetManualResolutionScale(float)
/* @confidence: H — all nine instructions establish the clamp order, constants and global store. */
#include <cmath>

namespace NRadEngine::CGRenderConfig {
namespace nevr_reconstruction {
extern float g_manualResolutionScale; // DAT_0389c1f0; ELF VA 0x379c1f0 at base 0x100000
}

void SetManualResolutionScale(float scale) {
    scale = std::fmax(scale, 0.01F); // fmaxnm with the 0.01f immediate
    nevr_reconstruction::g_manualResolutionScale =
        std::fmin(scale, 1.0F);      // fminnm; stored at ELF VA 0x379c1f0
}

} // namespace NRadEngine::CGRenderConfig
