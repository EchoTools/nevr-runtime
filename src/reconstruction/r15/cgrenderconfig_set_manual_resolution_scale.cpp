// @0x198d290 — NRadEngine::CGRenderConfig::SetManualResolutionScale(float)
/* @confidence: H — all nine instructions establish the clamp order, constants and global store. */
#include <cmath>

namespace NRadEngine::CGRenderConfig {
namespace reconstruction_detail {
extern float g_manualResolutionScale; // DAT_0389c1f0; ELF VA 0x379c1f0 at base 0x100000
}

void SetManualResolutionScale(float scale) {
    scale = std::fmax(scale, 0.01F); // fmaxnm with the 0.01f immediate
    reconstruction_detail::g_manualResolutionScale =
        std::fmin(scale, 1.0F);      // fminnm; stored at ELF VA 0x379c1f0
}

} // namespace NRadEngine::CGRenderConfig
