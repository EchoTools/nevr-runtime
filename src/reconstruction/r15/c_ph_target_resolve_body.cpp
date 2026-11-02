// @0x1aa27ac — NRadEngine::CPhTarget::ResolveBody
/* @confidence: H */
// Quest libr15.so VA 0x1aa27ac (RVA 0x19a27ac).
// ReVault binary 576, raw parent 100263899, reconstruction node 100600285; tier: reconstructed; grade: H, assessed by revault-codex.

#include <cstddef>
#include <cstdint>
#include "c_ph_target.hpp"

namespace NRadEngine {

struct CPhBody;


static_assert(offsetof(CPhTarget, body_index) == 0x08);
static_assert(offsetof(CPhTarget, zone_index) == 0x0c);
static_assert(offsetof(CPhTarget, generation) == 0x10);

struct CPhSystem {
    std::byte opaque_00[0x08];
    std::byte* zones;            // +0x08
    std::byte opaque_10[0x28];
    std::uint64_t zone_count;    // +0x38
};
static_assert(offsetof(CPhSystem, zones) == 0x08);
static_assert(offsetof(CPhSystem, zone_count) == 0x38);

extern CPhSystem* g_current_physics_system;

struct CPhZone {
    std::byte opaque_000[0x268];
    std::byte* bodies;           // +0x268
    std::byte opaque_270[0x28];
    std::uint64_t body_count;    // +0x298
};
static_assert(offsetof(CPhZone, bodies) == 0x268);
static_assert(offsetof(CPhZone, body_count) == 0x298);

struct CPhBodySlot {
    std::byte opaque_00[0x08];
    std::int32_t live_tag;       // +0x08; -1 means the slot is free
    std::uint32_t opaque_0c;
    std::int32_t generation;     // +0x10
};

CPhBody* CPhTarget::ResolveBody() const {
    const std::uint32_t index = body_index;
    if (index == UINT32_MAX) {
        return nullptr;
    }

    constexpr std::size_t kZoneStride = 0x1e20;
    constexpr std::size_t kBodyStride = 0xf38;
    auto* const zone_bytes = g_current_physics_system->zones +
                             static_cast<std::size_t>(zone_index) * kZoneStride;
    auto* const zone = reinterpret_cast<CPhZone*>(zone_bytes);
    if (static_cast<std::uint64_t>(index) >= zone->body_count) {
        return nullptr;
    }

    auto* const body_bytes = zone->bodies +
                            static_cast<std::size_t>(index) * kBodyStride;
    auto* const slot = reinterpret_cast<CPhBodySlot*>(body_bytes);
    if (slot->live_tag == -1) {
        return nullptr;
    }
    if (slot->generation != generation) {
        return nullptr;
    }
    return reinterpret_cast<CPhBody*>(body_bytes);
}

} // namespace NRadEngine
