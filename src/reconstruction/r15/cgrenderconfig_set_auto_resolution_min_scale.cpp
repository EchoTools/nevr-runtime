// @0x198d2f0 — NRadEngine::CGRenderConfig::SetAutoResolutionMinScale(float)
/* @confidence: H — all nine instructions establish fmaxnm(value, 0.01f), fminnm(value, 1.0f), and the store. */
#include <cmath>

namespace NRadEngine::CGRenderConfig {
namespace reconstruction_detail {
extern float g_autoResolutionMinScale; // DAT_0389c1f8; ELF VA 0x389c1f8
}

void SetAutoResolutionMinScale(float scale) {
    scale = std::fmax(scale, 0.01F);
    reconstruction_detail::g_autoResolutionMinScale = std::fmin(scale, 1.0F);
}
} // namespace NRadEngine::CGRenderConfig
