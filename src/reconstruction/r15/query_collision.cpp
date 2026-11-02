// query_collision
// Quest libr15.so VA 0x1906784; ReVault binary 576.
// ReVault raw parent 100256019, reconstruction node 100600251; grade H: Quest body pins the request hit field and root physics target lookup.
// @0x1906784 — NRadEngine::CncaGameSpace::QueryCollision
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CPhZone;

struct CPhTarget {
    std::uint64_t opaque_00;
    std::uint32_t zone_index;
    std::uint32_t opaque_0c;
    std::int32_t generation;
    std::uint32_t opaque_14;
    CPhZone* ResolveZone() const;
};
static_assert(sizeof(CPhTarget) == 0x18);

struct SPhQueryCollisionRequest {
    std::byte opaque_00[0x88];
    std::int32_t hit_body_id;
};
static_assert(offsetof(SPhQueryCollisionRequest, hit_body_id) == 0x88);

struct CPhZone {
    void QueryCollisionParticle(SPhQueryCollisionRequest& request,
                                std::uint32_t body_id);
};

// DAT_0386ce08 is assigned the CEngine instance by CncaGame::InitEngine.
// CEngine::Initialize stores InitRootNode3D() at Quest offset +0x16da8.
extern std::byte* g_engine_instance;

struct CncaGameSpace {
    bool QueryCollision(SPhQueryCollisionRequest& request,
                        std::uint32_t body_id) const;
};

bool CncaGameSpace::QueryCollision(SPhQueryCollisionRequest& request,
                                   std::uint32_t body_id) const {
    std::byte* const engine = g_engine_instance;
    std::byte* const root_physics_node =
        *reinterpret_cast<std::byte**>(engine + 0x16da8);

    CPhTarget target;
    std::memcpy(&target, root_physics_node + 0x98, sizeof(target));

    CPhZone* const zone = target.ResolveZone();
    zone->QueryCollisionParticle(request, body_id);
    return request.hit_body_id != -1;
}

} // namespace NRadEngine
