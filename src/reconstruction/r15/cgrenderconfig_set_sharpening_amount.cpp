// NRadEngine::CGRenderConfig::SetSharpeningAmount
// Quest libr15.so VA 0x198c984; ReVault binary 576.
// ReVault raw parent 100258486, reconstruction node 100600556; grade H: the node's @confidence is H, and its comments tie the clamp to the sign-bit test (fcsel on the integer bit pattern) at 0x198c984.
// @0x198c984 — NRadEngine::CGRenderConfig::SetSharpeningAmount
/* @confidence: H */
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// Sharpening amount at 0x389c098 (str s0,[x8,#0x98] at 0x198c998).
extern float g_sharpeningAmount;

class CGRenderConfig {
 public:
  static void SetSharpeningAmount(float amount);
};

// 0x198c984: fmov w8,s0  -- the float's bit pattern as a signed int
//            cmp w8,#0
//            fmov s1,wzr -- +0.0f
//            fcsel s0,s1,s0,lt -- sign bit set ? 0.0f : amount
//            str s0,[x8,#0x98]
// The test is on the integer bit pattern, not a float compare, so -0.0f and any
// other sign-bit-set value clamp to +0.0f. That is what memcpy plus the signed
// comparison reproduces.
void CGRenderConfig::SetSharpeningAmount(float amount) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &amount, sizeof(bits));
  g_sharpeningAmount =
      (static_cast<std::int32_t>(bits) < 0) ? 0.0f : amount;
}

}  // namespace NRadEngine
