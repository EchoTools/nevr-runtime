// NRadEngine::CGRenderConfig::SetTemporalAABlendWeight
// Quest libr15.so VA 0x198c91c; ReVault binary 576.
// ReVault raw parent 100258480, reconstruction node 100600555; grade H: the node's @confidence is H, and its comments tie the clamp to the fmaxnm/fminnm pair at 0x198c91c (0.0f to 1.0f).
// @0x198c91c — NRadEngine::CGRenderConfig::SetTemporalAABlendWeight
/* @confidence: H */
#include <cmath>
#include <cstdint>

namespace NRadEngine {

// Temporal AA blend weight at 0x389c08c (str s0,[x8,#0x8c] at 0x198c930).
extern float g_temporalAABlendWeight;

class CGRenderConfig {
 public:
  static void SetTemporalAABlendWeight(float weight);
};

// 0x198c91c: s1 = +0.0f, s2 = 1.0f, fmaxnm s0,s0,s1, fminnm s0,s0,s2.
// std::fmax/std::fmin are the maxNum/minNum operations the listing uses.
void CGRenderConfig::SetTemporalAABlendWeight(float weight) {
  g_temporalAABlendWeight = std::fmin(std::fmax(weight, 0.0f), 1.0f);
}

}  // namespace NRadEngine
