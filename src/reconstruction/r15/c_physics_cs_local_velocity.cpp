// NRadEngine::CPhysicsCS::LocalVelocity
// Quest libr15.so VA 0x1777214; ReVault binary 576.
// ReVault raw parent 100247449, reconstruction node 100600699; grade H: all 16 instructions checked to the ret at 0x1777250; no callee or global, so 0 placeholders; the record+0x20 pointee has no ReVault layout and is read at raw +0xc8; syntax-checked with mingw g++.
// @0x1777214 — NRadEngine::CPhysicsCS::LocalVelocity
/* @confidence: H */

#include <cstddef>
#include <cstdint>

namespace NRadEngine {

struct C3Vector {
    float x;
    float y;
    float z;
};
static_assert(sizeof(C3Vector) == 0x0c);

using SCompactPoolHandle = std::uint64_t;

// CPrimaryBodyRecord: 0x78-byte record of CPhysicsCS::primary_bodies. Only the record size and the
// offsets of body_index_map (0xc8) and primary_bodies (0xf8) agree with c_physics_cs_primary_body.cpp;
// the field breakdown here covers only what LocalVelocity reads (+0x0c and +0x20), the rest is opaque.
struct alignas(8) CPrimaryBodyRecord {
    std::byte opaque_00[0x0c];
    C3Vector vec_0c;          // +0x0c: three floats, read as one vector
    std::byte opaque_18[0x08];
    std::byte* opaque_20;     // +0x20: pointer to an object ReVault has no layout for
    std::byte opaque_28[0x50];
};
static_assert(sizeof(CPrimaryBodyRecord) == 0x78);
static_assert(offsetof(CPrimaryBodyRecord, vec_0c) == 0x0c);
static_assert(offsetof(CPrimaryBodyRecord, opaque_20) == 0x20);

// Only the members this function reads. The CComponentSystem base at +0x0 is part of opaque_000,
// not modelled. Only the offsets of body_index_map and primary_bodies agree with c_physics_cs_primary_body.cpp.
struct CPhysicsCS {
    std::byte opaque_000[0xc8];
    std::byte* body_index_map;            // +0xc8: u16 record index read at (handle & 0xffff) * 4
    std::byte opaque_0d0[0x28];
    CPrimaryBodyRecord* primary_bodies;   // +0xf8: array of 0x78-byte records

    C3Vector LocalVelocity(SCompactPoolHandle compact_handle) const;
};
static_assert(offsetof(CPhysicsCS, body_index_map) == 0xc8);
static_assert(offsetof(CPhysicsCS, primary_bodies) == 0xf8);

C3Vector CPhysicsCS::LocalVelocity(SCompactPoolHandle compact_handle) const {
    const auto* const mapping = reinterpret_cast<const std::uint16_t*>(
        body_index_map + (compact_handle & 0xffffu) * 4u);
    const CPrimaryBodyRecord* const record = primary_bodies + *mapping;

    // The pointee's C3Vector at +0xc8 minus the record's C3Vector at +0x0c, lane by lane.
    const std::byte* const node = record->opaque_20;
    const C3Vector& node_vec = *reinterpret_cast<const C3Vector*>(node + 0xc8);
    const C3Vector& record_vec = record->vec_0c;
    return C3Vector{node_vec.x - record_vec.x,
                    node_vec.y - record_vec.y,
                    node_vec.z - record_vec.z};
}

} // namespace NRadEngine
