// resolve_zone
// Quest libr15.so VA 0x1aa2668; ReVault binary 576.
// ReVault raw parent 100263895, reconstruction node 100600249; grade H: Quest body pins CPhTarget fields, bounds check, zone stride and generation check.
// @0x1aa2668 — NRadEngine::CPhTarget::ResolveZone
/* @confidence: H */

#include <cstddef>
#include <cstdint>

namespace NRadEngine {

struct CPhZone;

// The live physics system is assigned to DAT_0386cb38 by CEngine::InitPhysics
// and cleared by CEngine::ShutdownPhysics.
struct CPhSystem {
    std::byte opaque_000[0x08];
    std::byte* zones;                 // +0x08
    std::byte opaque_010[0x28];
    std::uint64_t zone_count;         // +0x38
};
static_assert(offsetof(CPhSystem, zones) == 0x08);
static_assert(offsetof(CPhSystem, zone_count) == 0x38);

extern CPhSystem* g_current_physics_system;

struct CPhTarget {
    std::uint64_t opaque_00;
    std::uint32_t zone_index;         // +0x08
    std::uint32_t opaque_0c;
    std::int32_t generation;          // +0x10
    std::uint32_t opaque_14;

    CPhZone* ResolveZone() const;
};
static_assert(sizeof(CPhTarget) == 0x18);
static_assert(offsetof(CPhTarget, zone_index) == 0x08);
static_assert(offsetof(CPhTarget, generation) == 0x10);

struct CPhZoneRecord {
    std::byte opaque_00[0x08];
    std::int32_t live_tag;            // +0x08
    std::uint32_t opaque_0c;
    std::int32_t generation;          // +0x10
};

CPhZone* CPhTarget::ResolveZone() const {
    const std::uint32_t index = zone_index;
    if (index == UINT32_MAX) {
        return nullptr;
    }

    CPhSystem* const system = g_current_physics_system;
    if (static_cast<std::uint64_t>(index) >= system->zone_count) {
        return nullptr;
    }

    constexpr std::size_t kZoneStride = 0x1e20;
    auto* const zone = reinterpret_cast<std::byte*>(system->zones) +
                       static_cast<std::size_t>(index) * kZoneStride;
    auto* const record = reinterpret_cast<CPhZoneRecord*>(zone);

    if (record->live_tag == -1) {
        return nullptr;
    }
    if (record->generation != generation) {
        return nullptr;
    }
    return reinterpret_cast<CPhZone*>(zone);
}

} // namespace NRadEngine
