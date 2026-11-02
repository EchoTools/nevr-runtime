// NRadEngine::CGShape::NumVerticesForSolidSphere
// Quest libr15.so VA 0x2906a8c; ReVault binary 576.
// ReVault raw parent 100330628, reconstruction node 100600762; grade H: all 7 instructions (28 bytes, 0x2906a8c to the ret at 0x2906aa4) checked against the listing, with no call, branch, global or vtable access; the name and parameter type are ReVault's; 0 placeholders remain.
// @0x2906a8c — NRadEngine::CGShape::NumVerticesForSolidSphere
/* @confidence: H */

#include <cstdint>

namespace NRadEngine {

struct CGShape {
    // The one call edge ReVault records for this VA is from the thunk at 0xfe8340; the thunk is not in this file.
    static int NumVerticesForSolidSphere(unsigned long long segments);
};

}  // namespace NRadEngine

// Body, 7 instructions, in listing order:
//   and w8, w0, #0xffff                        seg16 = low 16 bits of the argument (only w0 is read)
//   mov w9, #0xfffe; add w9, w9, w8, LSR #1    bias = 0xfffe + (seg16 >> 1)
//   mul w8, w9, w8; and w8, w8, #0xffff        product16 = (bias * seg16) mod 2^16
//   add w0, w8, #2; ret                        return product16 + 2
int NRadEngine::CGShape::NumVerticesForSolidSphere(unsigned long long segments) {
    const std::uint32_t seg16 = static_cast<std::uint32_t>(segments) & 0xffffu;
    const std::uint32_t bias = 0xfffeu + (seg16 >> 1);
    const std::uint32_t product16 = (bias * seg16) & 0xffffu;
    return static_cast<int>(product16 + 2u);
}
