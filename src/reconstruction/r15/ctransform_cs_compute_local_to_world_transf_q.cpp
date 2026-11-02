// NRadEngine::CTransformCS::ComputeLocalToWorldTransfQ
// Quest libr15.so VA 0x17f6ef0; ReVault binary 576.
// ReVault raw parent 100250157, reconstruction node 100600589; grade M: all 36 instructions are checked against the listing and every name is a ReVault name, but the callee is reached through GOT slot 0x37f8e88 (the file holds 0x00ecfb40), so its binding to 0x1a4ebe4 is ReVault's call graph, not byte-checked; CTransformCS and pool-element offsets are raw.
// @0x17f6ef0 — NRadEngine::CTransformCS::ComputeLocalToWorldTransfQ
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// SCompactPoolHandle arrives in x1 as one 64-bit register; this function reads only its low 16 bits.
using SCompactPoolHandle = std::uint64_t;

// 32-byte transform, returned through x8. The result is filled as raw bytes.
struct CTransfQ {
    float values[8];
};
static_assert(sizeof(CTransfQ) == 0x20);

// 48-byte transform returned through x8 by CNode3D::ComputeLocalToWorldTransfQS (six 8-byte stores in its listing).
struct CTransfQS {
    float value[12];
};
static_assert(sizeof(CTransfQS) == 0x30);

// Only the method this function calls is declared; the layout of CNode3D is not recovered.
struct CNode3D {
    // NRadEngine::CNode3D::ComputeLocalToWorldTransfQS(NRadEngine::CNode3D const*) @ 0x1a4ebe4, reached through thunk 0x1042f40.
    CTransfQS ComputeLocalToWorldTransfQS() const;
};

class CTransformCS {
public:
    // @0x17f6ef0 (thunk 0x103d910).
    CTransfQ ComputeLocalToWorldTransfQ(SCompactPoolHandle handle) const;

    // The layout of CTransformCS is not recovered. raw covers the pointers this function reads at +0xc8 and +0xf8.
    alignas(8) std::byte raw[0x100];
};
static_assert(sizeof(CTransformCS) >= 0x100);

namespace {

// Bytes per pool element: the madd with #0x118 in the listing.
constexpr std::size_t kPoolElementStride = 0x118;

template <typename T>
T Load(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

// Copies `bytes` raw bytes into the result at `dst_offset`.
void CopyIntoResult(CTransfQ& result, std::size_t dst_offset, const std::byte* src, std::size_t bytes) {
    std::memcpy(reinterpret_cast<std::byte*>(result.values) + dst_offset, src, bytes);
}

}  // namespace

CTransfQ CTransformCS::ComputeLocalToWorldTransfQ(SCompactPoolHandle handle) const {
    // 0x17f6ef8-0x17f6f08: slot = u16 at this->[0xc8] + 4 * (handle & 0xffff).
    const std::byte* const self = raw;
    const std::byte* const slot_map = Load<const std::byte*>(self, 0xc8);
    const auto index = Load<std::uint16_t>(slot_map, static_cast<std::size_t>(handle & 0xffffULL) * 4);

    // 0x17f6f0c-0x17f6f14: element = this->[0xf8] + slot * 0x118.
    const std::byte* const pool = Load<const std::byte*>(self, 0xf8);
    const std::byte* const element = pool + static_cast<std::size_t>(index) * kPoolElementStride;

    CTransfQ result{};
    // 0x17f6f18-0x17f6f1c: node = element->[0x8]; a null node takes the branch at 0x17f6f4c.
    const CNode3D* const node = Load<const CNode3D*>(element, 0x08);
    if (node != nullptr) {
        // 0x17f6f20-0x17f6f24: the callee writes a 48-byte result through x8 (a temporary at sp).
        const CTransfQS local = node->ComputeLocalToWorldTransfQS();
        // 0x17f6f28-0x17f6f3c: the first 32 bytes of the temporary are copied to the result, then return.
        CopyIntoResult(result, 0x00, reinterpret_cast<const std::byte*>(local.value), 0x20);
        return result;
    }

    // 0x17f6f4c-0x17f6f7c: no node; the result is copied from the element's own transform.
    CopyIntoResult(result, 0x00, element + 0xe8, 16);  // q0 = element[0xe8..0xf7]
    CopyIntoResult(result, 0x10, element + 0xf8, 8);   // x10 = element[0xf8..0xff]
    CopyIntoResult(result, 0x18, element + 0x100, 4);  // w9  = element[0x100..0x103]
    CopyIntoResult(result, 0x1c, element + 0x104, 4);  // w8  = element[0x104..0x107]
    return result;
}

}  // namespace NRadEngine
