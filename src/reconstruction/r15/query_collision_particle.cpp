// query_collision_particle
// Quest libr15.so VA 0x1aecee8; ReVault binary 576.
// ReVault raw parent 100264983, reconstruction node 100600250; grade H: Quest body is a direct call-through to RayQuery.
// @0x1aecee8 — NRadEngine::CPhZone::QueryCollisionParticle
/* @confidence: H */

#include <cstdint>

namespace NRadEngine {

struct SPhQueryCollisionRequest;

struct CPhZone {
    void QueryCollisionParticle(SPhQueryCollisionRequest& request,
                                std::uint32_t body_id);
    void RayQuery(SPhQueryCollisionRequest& request, std::uint32_t body_id);
};

void CPhZone::QueryCollisionParticle(SPhQueryCollisionRequest& request,
                                     std::uint32_t body_id) {
    RayQuery(request, body_id);
}

} // namespace NRadEngine
