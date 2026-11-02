// NRadEngine::CGShape::NumIndicesForSolidSphere
// Quest libr15.so VA 0x2906a44; ReVault binary 576.
// ReVault raw parent 100330625, reconstruction node 100600763; grade H: all 10 instructions (0x2906a44 to 0x2906a68, 40 bytes) are mapped one-to-one in the body; leaf function with no callees or globals, no placeholders.
// @0x2906a44 — NRadEngine::CGShape::NumIndicesForSolidSphere
/* @confidence: H */
#include <cstdint>

namespace NRadEngine {
struct CGShape {
    static int NumIndicesForSolidSphere(unsigned long long segments);
};
}  // namespace NRadEngine

int NRadEngine::CGShape::NumIndicesForSolidSphere(unsigned long long segments) {
    // 0x2906a44: and w8, w0, #0xffff
    const std::uint32_t slices = static_cast<std::uint32_t>(segments) & 0xffffu;
    // 0x2906a48: mov w9, #0xfffd; 0x2906a4c: add w9, w9, w8, LSR #1
    const std::uint32_t half_plus = 0xfffdu + (slices >> 1);
    // 0x2906a50: mul w8, w8, w9 (32-bit, wraps)
    const std::uint32_t product = slices * half_plus;
    // 0x2906a54: add w10, w0, w0, LSL #1 (3 times the 32-bit argument)
    const std::uint32_t arg_times3 = static_cast<std::uint32_t>(segments) * 3u;
    // 0x2906a58: add w8, w8, w8, LSL #1 (3 times the product)
    const std::uint32_t product_times3 = product * 3u;
    // 0x2906a5c: ubfiz w8, w8, #1, #15 ((x & 0x7fff) << 1)
    const std::uint32_t term_a = (product_times3 & 0x7fffu) << 1;
    // 0x2906a60: ubfiz w9, w10, #1, #15 ((x & 0x7fff) << 1)
    const std::uint32_t term_b = (arg_times3 & 0x7fffu) << 1;
    // 0x2906a64: add w0, w8, w9; 0x2906a68: ret
    return static_cast<int>(term_a + term_b);
}
