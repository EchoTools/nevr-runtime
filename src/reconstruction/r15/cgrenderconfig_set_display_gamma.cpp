// NRadEngine::CGRenderConfig::SetDisplayGamma
// Quest libr15.so VA 0x198c7f4; ReVault binary 576.
// ReVault raw parent 100258463, reconstruction node 100600554; grade H: the node's @confidence is H, and its comments tie the clamp to the fmaxnm/fminnm pair at 0x198c7f4 (-1.0f to 1.0f).
// @0x198c7f4 — NRadEngine::CGRenderConfig::SetDisplayGamma
/* @confidence: H */
#include <cmath>
#include <cstdint>

namespace NRadEngine {

// Display gamma at 0x389c078 (str s0,[x8,#0x78] at 0x198c808).
extern float g_displayGamma;

class CGRenderConfig {
 public:
  static void SetDisplayGamma(float gamma);
};

// 0x198c7f4: s1 = -1.0f, s2 = 1.0f, fmaxnm s0,s0,s1, fminnm s0,s0,s2.
// std::fmax/std::fmin are the IEEE maxNum/minNum operations the listing uses
// (the "nm" forms), so NaN handling matches.
void CGRenderConfig::SetDisplayGamma(float gamma) {
  g_displayGamma = std::fmin(std::fmax(gamma, -1.0f), 1.0f);
}

}  // namespace NRadEngine
