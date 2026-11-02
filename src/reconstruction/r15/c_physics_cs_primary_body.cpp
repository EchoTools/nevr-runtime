// @0x1777d34 — NRadEngine::CPhysicsCS::PrimaryBody
/* @confidence: H */
// Quest libr15.so VA 0x1777d34 (RVA 0x1677d34).
// ReVault binary 576, raw parent 100247466, reconstruction node 100600284; tier: reconstructed; grade: H, assessed by revault-codex.

#include <cstddef>
#include <cstdint>
#include "c_ph_target.hpp"

namespace NRadEngine {


static_assert(sizeof(CPhTarget) == 0x18);
static_assert(offsetof(CPhTarget, body_index) == 0x08);
static_assert(offsetof(CPhTarget, zone_index) == 0x0c);
static_assert(offsetof(CPhTarget, generation) == 0x10);

struct CPhysicsState {
    std::byte opaque_000[0xc40];
    std::uint8_t flags;          // +0xc40
};
static_assert(offsetof(CPhysicsState, flags) == 0xc40);

struct CPrimaryBodyRecord {
    std::byte opaque_00[0x38];
    std::uint64_t valid;         // +0x38
    std::uint64_t handle_00;     // +0x40
    std::uint64_t handle_08;     // +0x48
    std::uint64_t handle_10;     // +0x50
    std::byte opaque_58[0x20];
};
static_assert(sizeof(CPrimaryBodyRecord) == 0x78);
static_assert(offsetof(CPrimaryBodyRecord, valid) == 0x38);
static_assert(offsetof(CPrimaryBodyRecord, handle_00) == 0x40);
static_assert(offsetof(CPrimaryBodyRecord, handle_08) == 0x48);
static_assert(offsetof(CPrimaryBodyRecord, handle_10) == 0x50);

struct CPhysicsCS {
    std::byte opaque_000[0x80];
    CPhysicsState* state;                // +0x80
    std::byte opaque_088[0x40];
    std::byte* body_index_map;           // +0xc8
    std::byte opaque_0d0[0x24];
    std::uint16_t primary_body_count;    // +0xf4
    std::uint16_t opaque_0f6;
    CPrimaryBodyRecord* primary_bodies;  // +0xf8

    CPhTarget PrimaryBody(std::uint64_t compact_handle) const;
};
static_assert(offsetof(CPhysicsCS, state) == 0x80);
static_assert(offsetof(CPhysicsCS, body_index_map) == 0xc8);
static_assert(offsetof(CPhysicsCS, primary_body_count) == 0xf4);
static_assert(offsetof(CPhysicsCS, primary_bodies) == 0xf8);

CPhTarget CPhysicsCS::PrimaryBody(std::uint64_t compact_handle) const {
    const auto* const mapping = reinterpret_cast<const std::uint16_t*>(
        body_index_map + (compact_handle & 0xffffu) * 4u);
    const std::uint16_t body_record_index = *mapping;
    const CPrimaryBodyRecord* const record = primary_bodies + body_record_index;

    if (((state->flags & 1u) != 0u || body_record_index < primary_body_count) &&
        record->valid != 0u) {
        CPhTarget result;
        result.opaque_00 = record->handle_00;
        result.body_index = static_cast<std::uint32_t>(record->handle_08);
        result.zone_index = static_cast<std::uint32_t>(record->handle_08 >> 32);
        result.generation = static_cast<std::int32_t>(record->handle_10);
        result.opaque_14 = static_cast<std::uint32_t>(record->handle_10 >> 32);
        return result;
    }

    CPhTarget result;
    result.opaque_00 = UINT64_MAX;
    result.body_index = UINT32_MAX;
    result.zone_index = UINT32_MAX;
    result.generation = 0;
    result.opaque_14 = UINT32_MAX;
    result.ValidateTag();
    return result;
}

} // namespace NRadEngine
