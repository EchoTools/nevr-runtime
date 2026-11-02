// world_teleport
// Quest libr15.so VA 0x190a384; ReVault binary 576.
// ReVault raw parent 100256127, reconstruction node 100600243; grade H: Quest instructions pin transform fields and the LocalTeleport call ABI.
#include <cstdint>
#include <cstddef>

namespace NRadEngine {

struct SActor { std::uint64_t bits; };
static_assert(sizeof(SActor) == 0x8);

struct CTransfQ { float values[8]; };
static_assert(sizeof(CTransfQ) == 0x20);

enum class EComponentTeleportReason : std::uint32_t {};

class CncaGameSpace {
public:
    void WorldTeleport(SActor actor, const CTransfQ& pose,
                       EComponentTeleportReason reason);
    void LocalTeleport(SActor actor, const CTransfQ& pose,
                       EComponentTeleportReason reason);
};

// @0x190a384 — NRadEngine::CncaGameSpace::WorldTeleport
/* @confidence: H */
void CncaGameSpace::WorldTeleport(
    SActor actor,
    const CTransfQ& pose,
    EComponentTeleportReason reason)
{
    const auto self = reinterpret_cast<std::uintptr_t>(this);
    const float qx = *reinterpret_cast<const float*>(self + 0xd0);
    const float qy = *reinterpret_cast<const float*>(self + 0xd4);
    const float qz = *reinterpret_cast<const float*>(self + 0xd8);
    const float qw = *reinterpret_cast<const float*>(self + 0xdc);
    const float scale = *reinterpret_cast<const float*>(self + 0xec);

    const float px = pose.values[0], py = pose.values[1];
    const float pz = pose.values[2], pw = pose.values[3];
    const float tx = pose.values[4], ty = pose.values[5];
    const float tz = pose.values[6], pose_scale = pose.values[7];

    const float cross_x = tz*qy - ty*qz;
    const float cross_y = ty*qx - tx*qy;
    const float cross_z = tx*qz - tz*qx;
    const float twice_x = cross_x + cross_x;
    const float twice_y = cross_y + cross_y;
    const float twice_z = cross_z + cross_z;

    CTransfQ result{};
    result.values[0] = ((pw*qy + qz*px + qw*py) - qx*pz);
    result.values[1] = ((pw*qx + qw*px) - qz*py) + qy*pz;
    result.values[2] = (pw*qz - px*qy) + qx*py + qw*pz;
    result.values[3] = pw*qw - (px*qx + py*qy + qz*pz);
    result.values[4] = *reinterpret_cast<const float*>(self + 0xe0) +
        scale*((tx + qw*twice_x + qy*twice_y) - qz*twice_z);
    result.values[5] = *reinterpret_cast<const float*>(self + 0xe4) +
        scale*((ty + qz*twice_x + qw*twice_z) - qx*twice_y);
    result.values[6] = *reinterpret_cast<const float*>(self + 0xe8) +
        scale*((tz - qy*twice_x) + qx*twice_z + qw*twice_y);
    result.values[7] = pose_scale*scale;

    LocalTeleport(actor, result, reason);
}

}
