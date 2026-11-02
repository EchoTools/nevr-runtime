// NRadEngine::CTransformCS::ComputeParentToWorldTransfQ(SCompactPoolHandle) const
// Quest libr15.so VA 0x181e00c; ReVault binary 576.
// ReVault raw parent 100250809, reconstruction node 100600703; grade M: the pool lookup, parent-node transform path, and null-node level-to-world path match all 32 instructions; transform records are copied by their observed byte widths.
// @0x181e00c — NRadEngine::CTransformCS::ComputeParentToWorldTransfQ
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct SCompactPoolHandle {
    std::uint64_t value;
};
static_assert(sizeof(SCompactPoolHandle) == 0x08);

struct CTransfQ {
    std::byte bytes[0x20];
};
static_assert(sizeof(CTransfQ) == 0x20);

struct CTransfQS {
    std::byte bytes[0x30];
};
static_assert(sizeof(CTransfQS) == 0x30);

class CNode3D {
public:
    CTransfQS ComputeParentToWorldTransfQS() const;
};

class CncaGameSpace {
public:
    void ComputeLevelToWorld(CTransfQ& out) const;
};

class CTransformCS {
public:
    CTransfQ ComputeParentToWorldTransfQ(SCompactPoolHandle handle) const;
};

}  // namespace NRadEngine

namespace {

template <typename T>
T Load(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

}  // namespace

namespace NRadEngine {

CTransfQ CTransformCS::ComputeParentToWorldTransfQ(SCompactPoolHandle handle) const {
    const auto* const self = reinterpret_cast<const std::byte*>(this);
    const auto* const handle_indices = Load<const std::byte*>(self, 0xc8);
    const auto index = Load<std::uint16_t>(
        handle_indices, static_cast<std::size_t>(handle.value & 0xffffU) * 4);
    const auto* const pool_entries = Load<const std::byte*>(self, 0xf8);
    const auto* const entry = pool_entries + static_cast<std::size_t>(index) * 0x118;
    const auto* const parent = Load<const CNode3D*>(entry, 0x08);
    CTransfQ result{};

    if (parent != nullptr) {
        const CTransfQS parent_to_world = parent->ComputeParentToWorldTransfQS();
        std::memcpy(&result, &parent_to_world, sizeof(result));
        return result;
    }

    const std::uint64_t identity_words[] = {
        0,
        0x3f80000000000000ULL,
        0,
        0x3f80000000000000ULL,
    };
    std::memcpy(&result, identity_words, sizeof(identity_words));
    Load<CncaGameSpace*>(self, 0x80)->ComputeLevelToWorld(result);
    return result;
}

}  // namespace NRadEngine
