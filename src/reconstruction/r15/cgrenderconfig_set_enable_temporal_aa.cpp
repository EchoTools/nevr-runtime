// NRadEngine::CGRenderConfig::SetEnableTemporalAA
// Quest libr15.so VA 0x198c8f0; ReVault binary 576.
// ReVault raw parent 100258476, reconstruction node 100600552; grade H: the node's @confidence is H, and its comments trace the 12 bytes (adrp, str, ret) at 0x198c8f0..0x198c8f8 with no truncation.
// @0x198c8f0 — NRadEngine::CGRenderConfig::SetEnableTemporalAA
/* @confidence: H */
#include <cstdint>

namespace NRadEngine {

// Render-config storage at 0x389c084: adrp 0x389c000 (0x198c8f0) then
// str w0,[x8,#0x84] (0x198c8f4), ret at 0x198c8f8. 12 bytes exactly, no truncation.
extern std::uint32_t g_enableTemporalAA;

class CGRenderConfig {
 public:
  static void SetEnableTemporalAA(std::uint32_t enabled);
};

void CGRenderConfig::SetEnableTemporalAA(std::uint32_t enabled) {
  g_enableTemporalAA = enabled;
}

}  // namespace NRadEngine
