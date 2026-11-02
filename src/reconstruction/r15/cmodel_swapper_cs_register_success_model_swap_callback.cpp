// NRadEngine::CModelSwapperCS::RegisterSuccessModelSwapCallback
// Quest libr15.so VA 0x1734f00; ReVault binary 576.
// ReVault raw parent 100246517, reconstruction node 100600696; grade M: all 10 instructions are checked against the listing (one straight-line path, no callee, no global, 0 placeholders); arg4 and arg5 are unrecovered parameter labels, and this+0xd0, this+0x100 and the record fields +0xc8..+0xe0 are raw offsets; syntax-checked.
// @0x1734f00 — NRadEngine::CModelSwapperCS::RegisterSuccessModelSwapCallback
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// SCompactPoolHandle arrives in x1 as one 64-bit register; this function reads only its low 16 bits.
using SCompactPoolHandle = std::uint64_t;

// The class derives from CComponentSystem at +0x0 (ReVault class hierarchy); its layout is not recovered here.
class CModelSwapperCS {
public:
    // @0x1734f00 (thunk 0x104aa10). Not in the vtable at 0x374bd30: a non-virtual member.
    // arg4 and arg5 are labels: the callee's parameter names are not recovered.
    void RegisterSuccessModelSwapCallback(SCompactPoolHandle handle,
                                          void (*callback)(void*, std::uint64_t, std::uint64_t),
                                          void* context, std::uint64_t arg4, std::uint64_t arg5);

    // Only the two pointers this function reads are recovered: +0xd0 (slot map), +0x100 (record array).
    alignas(8) std::byte raw[0x108];
};
static_assert(sizeof(CModelSwapperCS) >= 0x108);

namespace {

// Bytes per record: the madd with #0x1d8 in the listing.
constexpr std::size_t kRecordStride = 0x1d8;

template <typename T>
T Load(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

template <typename T>
void Store(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof value);
}

}  // namespace

void CModelSwapperCS::RegisterSuccessModelSwapCallback(SCompactPoolHandle handle,
                                                       void (*callback)(void*, std::uint64_t, std::uint64_t),
                                                       void* context, std::uint64_t arg4, std::uint64_t arg5) {
    std::byte* const self = raw;

    // 0x1734f00-0x1734f08: ldr x8, [x0, #0xd0]; and x9, x1, #0xffff; lsl x9, x9, #2.
    // The slot is the u16 at slot_map + 4 * (handle & 0xffff).
    const std::byte* const slot_map = Load<const std::byte*>(self, 0xd0);
    const auto slot = Load<std::uint16_t>(slot_map, static_cast<std::size_t>(handle & 0xffffULL) * 4);

    // 0x1734f0c-0x1734f18: ldr x10, [x0, #0x100]; ldrh w8, [x8, x9]; mov w9, #0x1d8; madd x8, x8, x9, x10.
    // The record is records + slot * 0x1d8.
    std::byte* const records = Load<std::byte*>(self, 0x100);
    std::byte* const record = records + static_cast<std::size_t>(slot) * kRecordStride;

    // 0x1734f1c: stp x2, x3, [x8, #0xc8]: callback at +0xc8, context at +0xd0.
    Store(record, 0xc8, callback);
    Store(record, 0xd0, context);
    // 0x1734f20: stp x4, x5, [x8, #0xd8]: arg4 at +0xd8, arg5 at +0xe0, both full 64-bit stores; ret at 0x1734f24.
    Store(record, 0xd8, arg4);
    Store(record, 0xe0, arg5);
}

}  // namespace NRadEngine
