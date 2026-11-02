// NRadEngine::CGRenderConfig::SetMSAAMode
// Quest libr15.so VA 0x198c8a0; ReVault binary 576.
// ReVault raw parent 100258471, reconstruction node 100600551; grade H: the node's @confidence is H, and its comments trace the 12 bytes (adrp, str, ret) at 0x198c8a0..0x198c8a8 with no truncation.
// @0x198c8a0 — NRadEngine::CGRenderConfig::SetMSAAMode
/* @confidence: H */
#include <cstdint>

namespace NRadEngine {

enum EMSAAMode : std::uint32_t;

// Render-config storage at 0x389c080: adrp 0x389c000 (0x198c8a0) then
// str w0,[x8,#0x80] (0x198c8a4), ret at 0x198c8a8. 12 bytes exactly, no truncation.
extern std::uint32_t g_msaaMode;

class CGRenderConfig {
 public:
  static void SetMSAAMode(EMSAAMode mode);
};

void CGRenderConfig::SetMSAAMode(EMSAAMode mode) {
  g_msaaMode = static_cast<std::uint32_t>(mode);
}

}  // namespace NRadEngine
