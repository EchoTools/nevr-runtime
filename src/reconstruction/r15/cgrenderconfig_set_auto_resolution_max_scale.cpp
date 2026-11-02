// @0x198d320 — NRadEngine::CGRenderConfig::SetAutoResolutionMaxScale(float)
/* @confidence: H — all nine instructions establish fmaxnm(value, 0.01f), fminnm(value, 1.0f), and the store. */
#include <cmath>

namespace NRadEngine::CGRenderConfig {
namespace reconstruction_detail {
extern float g_autoResolutionMaxScale; // DAT_0389c1fc; ELF VA 0x389c1fc
}

void SetAutoResolutionMaxScale(float scale) {
    scale = std::fmax(scale, 0.01F);
    reconstruction_detail::g_autoResolutionMaxScale = std::fmin(scale, 1.0F);
}
} // namespace NRadEngine::CGRenderConfig
