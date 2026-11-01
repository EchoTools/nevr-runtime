// @0x198d2b4 — NRadEngine::CGRenderConfig::SetEnableAutoResolution(std::uint32_t)
/* @confidence: H — three instructions store the argument unchanged to the enable flag. */
#include <cstdint>

namespace NRadEngine::CGRenderConfig {
namespace reconstruction_detail {
extern std::uint32_t g_enableAutoResolution; // DAT_0389c1f4; ELF VA 0x389c1f4
}

void SetEnableAutoResolution(std::uint32_t enabled) {
    reconstruction_detail::g_enableAutoResolution = enabled;
}
} // namespace NRadEngine::CGRenderConfig
