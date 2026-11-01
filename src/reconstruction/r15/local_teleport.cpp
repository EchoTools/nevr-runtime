// local_teleport
// Quest libr15.so VA 0x1909f94; ReVault binary 576.
// ReVault raw parent 100256126, reconstruction node 100600245; grade L: the allocator TLS bytes at 0x38611b8 conflict with its pthread key use.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CncaGameSpace;

struct SActor { std::uint64_t bits; };
static_assert(sizeof(SActor) == 0x8);

struct CTransfQ { float values[8]; };
static_assert(sizeof(CTransfQ) == 0x20);

enum class EComponentTeleportReason : std::uint32_t {};

struct SComponentEvent { std::uint64_t words[5]; };
static_assert(sizeof(SComponentEvent) == 0x28);

// @0x14d6da0 tail-branches to @0x1461050; the target returns 0/1.
bool ResolveActorAlias(CncaGameSpace** game_space, SActor* actor);
// @0x12e5e14; raw body confirms the three uint32 arrays, uint64 count, shift.
void RadixSortIndices(std::uint32_t*, std::uint32_t*, std::uint32_t*,
                      std::uint64_t, std::uint32_t);

struct CSysThread {
    // @0x108a22c tail-branches to the imported pthread_getspecific.
    static void* GetTLS(int key);
};

class CncaGameSpace {
public:
    void WorldTeleport(SActor actor, const CTransfQ& pose,
                       EComponentTeleportReason reason);
    void LocalTeleport(SActor actor, const CTransfQ& pose,
                       EComponentTeleportReason reason);
    // @0x190281c; ARM64 uses x1=event, w2=flags.
    void SendComponentEventGlobal(const SComponentEvent& event,
                                  std::uint32_t flags);
};

// @0x1909f94 — NRadEngine::CncaGameSpace::LocalTeleport
/* @confidence: L */
/* The allocator TLS key at 0x38611b8 remains uncertain: stored bytes read as URL
   text although this path passes the loaded word to pthread_getspecific. */
extern std::uint32_t g_allocator_tls_index;

namespace {
using Address = std::uintptr_t;

template<class Fn>
Fn virtual_at(Address object, std::size_t byte_offset) {
    const Address table = *reinterpret_cast<const Address*>(object);
    return reinterpret_cast<Fn>(*reinterpret_cast<const Address*>(table + byte_offset));
}
}

void CncaGameSpace::LocalTeleport(
    SActor actor, const CTransfQ& pose, EComponentTeleportReason reason)
{
    const Address self = reinterpret_cast<Address>(this);
    Address resolved_game_space = self;
    bool alias_resolved = false;

    if (ResolveActorAlias(
            reinterpret_cast<CncaGameSpace**>(&resolved_game_space), &actor) &&
        resolved_game_space != 0) {
        const float qx = *reinterpret_cast<const float*>(self + 0xb0);
        const float qy = *reinterpret_cast<const float*>(self + 0xb4);
        const float qz = *reinterpret_cast<const float*>(self + 0xb8);
        const float qw = *reinterpret_cast<const float*>(self + 0xbc);
        const float scale = *reinterpret_cast<const float*>(self + 0xcc);

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

        CTransfQ combined{};
        combined.values[0] = ((pw*qy + qz*px + qw*py) - qx*pz);
        combined.values[1] = ((pw*qx + qw*px) - qz*py) + qy*pz;
        combined.values[2] = (pw*qz - px*qy) + qx*py + qw*pz;
        combined.values[3] = pw*qw - (px*qx + py*qy + qz*pz);
        combined.values[4] = *reinterpret_cast<const float*>(self + 0xc0) +
            scale*((tx + qw*twice_x + qy*twice_y) - qz*twice_z);
        combined.values[5] = *reinterpret_cast<const float*>(self + 0xc4) +
            scale*((ty + qz*twice_x + qw*twice_z) - qx*twice_y);
        combined.values[6] = *reinterpret_cast<const float*>(self + 0xc8) +
            scale*((tz - qy*twice_x) + qx*twice_z + qw*twice_y);
        combined.values[7] = pose_scale*scale;

        reinterpret_cast<CncaGameSpace*>(resolved_game_space)->WorldTeleport(
            actor, combined, reason);
        alias_resolved = true;
    }

    const std::uint64_t component_count =
        *reinterpret_cast<const std::uint64_t*>(self + 0x450);
    const std::uint32_t count = static_cast<std::uint32_t>(component_count);
    const Address tls =
        reinterpret_cast<Address>(CSysThread::GetTLS(static_cast<int>(g_allocator_tls_index)));

    std::uint32_t* priorities = nullptr;
    std::uint32_t* indices = nullptr;
    std::uint32_t* sorted = nullptr;
    if (count != 0) {
        auto allocate = virtual_at<Address(*)(Address, std::uint64_t)>(tls, 0x10);
        const std::uint64_t bytes = static_cast<std::uint64_t>(count) << 2;
        priorities = reinterpret_cast<std::uint32_t*>(allocate(tls, bytes));
        indices = reinterpret_cast<std::uint32_t*>(allocate(tls, bytes));
        sorted = reinterpret_cast<std::uint32_t*>(allocate(tls, bytes));
    }

    if (count != 0) {
        const Address component_array = *reinterpret_cast<const Address*>(self + 0x420);
        const std::uint64_t event_count =
            *reinterpret_cast<const std::uint64_t*>(self + 0x588);
        const Address event_table = *reinterpret_cast<const Address*>(self + 0x558);

        for (std::uint64_t i = 0; i != count; ++i) {
            Address event = self + 0x598;
            if (event_count != 0) {
                std::uint64_t low = 0, high = event_count;
                const std::int64_t key = *reinterpret_cast<const std::int64_t*>(
                    *reinterpret_cast<const Address*>(component_array + i*0x10 + 8) + 0x28);
                while (low < high) {
                    const std::uint64_t mid = low + (high - low)/2;
                    const std::int64_t candidate = *reinterpret_cast<const std::int64_t*>(
                        event_table + mid*0x10);
                    if (candidate < key) low = mid + 1;
                    else high = mid;
                }
                if (low < event_count &&
                    *reinterpret_cast<const std::int64_t*>(event_table + low*0x10) == key) {
                    event = event_table + low*0x10 + 8;
                }
            }
            priorities[i] = *reinterpret_cast<const std::uint16_t*>(event);
            indices[i] = static_cast<std::uint32_t>(i);
        }
    }

    RadixSortIndices(priorities, indices, sorted, count, 0);

    std::uint64_t event_actor = 0xffffffffffffULL;
    const std::uint64_t event_mask = 0xffffffffffffffffULL;
    if (count != 0) {
        const Address component_array = *reinterpret_cast<const Address*>(self + 0x420);
        for (std::uint64_t i = 0; i != count; ++i) {
            const Address component =
                *reinterpret_cast<const Address*>(component_array +
                    static_cast<std::uint64_t>(sorted[i])*0x10 + 8);
            virtual_at<void(*)(Address, std::uint64_t, std::uint64_t*)>(
                component, 0x2b0)(component, actor.bits, &event_actor);

            while (static_cast<std::int16_t>(event_actor) != -1) {
                const std::uint64_t event_index =
                    virtual_at<std::uint64_t(*)(Address, std::uint64_t, std::uint64_t)>(
                        component, 0x1b8)(component,
                            static_cast<std::uint16_t>(event_actor), event_mask);
                const std::uint64_t dispatch_id =
                    (component_count & 0xffffffff00000000ULL) |
                    (event_index & 0xffffffffULL);
                virtual_at<void(*)(Address, std::uint64_t, const CTransfQ&,
                                   EComponentTeleportReason)>(
                    component, 0x2c8)(component, dispatch_id, pose, reason);
                virtual_at<void(*)(Address, std::uint64_t*)>(
                    component, 0x2b8)(component, &event_actor);
            }
        }
    }

    if (!alias_resolved) {
        SComponentEvent event{};
        event.words[0] = 0x2d9a6b418f74edb2ULL;
        event.words[1] = *reinterpret_cast<const Address*>(self + 0x58);
        event.words[2] = actor.bits;
        event.words[3] = 0xffffffffffffffffULL;
        event.words[4] = 0xffffffffffffffffULL;
        SendComponentEventGlobal(event, 1);
    }

    if (tls + 1 > 1) {
        auto release = virtual_at<void(*)(Address, Address)>(tls, 0x38);
        if (sorted != nullptr) release(tls, reinterpret_cast<Address>(sorted));
        if (indices != nullptr) release(tls, reinterpret_cast<Address>(indices));
        if (priorities != nullptr) release(tls, reinterpret_cast<Address>(priorities));
    }
}

} // namespace NRadEngine
