// NRadEngine::CGRenderConfig::SetAutoResolutionFrameRate(std::uint32_t)
// Quest libr15.so VA 0x198d350; ReVault binary 576.
// ReVault raw parent 100258554, reconstruction node 100600230; grade H: the compare, conditional selection and global store match all five instructions.
// @0x198d350 — NRadEngine::CGRenderConfig::SetAutoResolutionFrameRate(std::uint32_t)
/* @confidence: H — compare, conditional selection, and global store match all five instructions. */
#include <cstdint>

namespace NRadEngine::CGRenderConfig {
namespace reconstruction_detail {
extern std::uint32_t g_autoResolutionFrameRate; // DAT_0389c200; ELF VA 0x389c200
}

void SetAutoResolutionFrameRate(std::uint32_t frameRate) {
    if (static_cast<std::int32_t>(frameRate) < 2) {
        frameRate = 1;
    }
    reconstruction_detail::g_autoResolutionFrameRate = frameRate;
}
} // namespace NRadEngine::CGRenderConfig
