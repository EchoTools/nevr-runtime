// NRadEngine::CTransformCS::ComputeLocalToLevelTransfQ(SCompactPoolHandle) const
// Quest libr15.so VA 0x17f6e4c; ReVault binary 576.
// ReVault raw parent 100250156, reconstruction node 100600702; grade M: index lookup and both pool-entry branches match all 41 instructions; transform and node types are represented as exact byte layouts.
// @0x17f6e4c — NRadEngine::CTransformCS::ComputeLocalToLevelTransfQ
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

class CNode3D;

class CncaGameSpace {
public:
    void ComputeLocalToLevel(CTransfQS& out, const CNode3D& node) const;
};

class CTransformCS {
public:
    CTransfQ ComputeLocalToLevelTransfQ(SCompactPoolHandle handle) const;
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

CTransfQ CTransformCS::ComputeLocalToLevelTransfQ(SCompactPoolHandle handle) const {
    const auto* const self = reinterpret_cast<const std::byte*>(this);
    const auto* const handle_indices = Load<const std::byte*>(self, 0xc8);
    const auto index = Load<std::uint16_t>(
        handle_indices, static_cast<std::size_t>(handle.value & 0xffffU) * 4);
    const auto* const pool_entries = Load<const std::byte*>(self, 0xf8);
    const auto* const entry = pool_entries + static_cast<std::size_t>(index) * 0x118;
    CTransfQ result{};

    if (Load<const CNode3D*>(entry, 0x08) == nullptr) {
        std::memcpy(&result, entry + 0xb8, sizeof(result));
        return result;
    }

    CTransfQS local_to_level{};
    const std::uint64_t identity_words[] = {
        0,
        0x3f80000000000000ULL,
        0,
        0x3f80000000000000ULL,
        0x3f8000003f800000ULL,
        0,
    };
    std::memcpy(&local_to_level, identity_words, sizeof(identity_words));

    auto* const game_space = Load<CncaGameSpace*>(self, 0x80);
    game_space->ComputeLocalToLevel(local_to_level, *Load<const CNode3D*>(entry, 0x08));
    std::memcpy(&result, &local_to_level, sizeof(result));
    return result;
}

}  // namespace NRadEngine
