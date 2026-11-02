// NRadEngine::CGRenderConfig::SetEnableAutoResolution(std::uint32_t)
// Quest libr15.so VA 0x198d2b4; ReVault binary 576.
// ReVault raw parent 100258547, reconstruction node 100600229; grade H: three instructions store the argument unchanged to the enable flag.
// @0x198d2b4 — NRadEngine::CGRenderConfig::SetEnableAutoResolution(std::uint32_t)
/* @confidence: H — three instructions store the argument unchanged to the enable flag. */
#include <cstdint>

namespace NRadEngine::CGRenderConfig {
namespace nevr_reconstruction {
extern std::uint32_t g_enableAutoResolution; // DAT_0389c1f4; ELF VA 0x389c1f4
}

void SetEnableAutoResolution(std::uint32_t enabled) {
    nevr_reconstruction::g_enableAutoResolution = enabled;
}
} // namespace NRadEngine::CGRenderConfig
