// SR15PatrolSM::Update(SR15NetAIContext&, SR15NetAgent&)
// Quest libr15.so VA 0x13c096c; ReVault binary 576.
// ReVault raw parent 100224992, reconstruction node 100600474; grade M: every branch and callee traced in the disassembly; the up vector behind the pointer at 0x3814e98 is load-time, so the basis is not byte-checked.
// @0x13c096c — SR15PatrolSM::Update
/* @confidence: M */
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CDeferredMethodQueue;
class CncaGameSpaceMT;

struct C3Vector { float v[3]; };
struct C33Matrix { float m[9]; };

struct CQuaternion {
    float v[4];
    void FromRotationMatrix(const C33Matrix& basis);  // CQuaternion::FromRotationMatrix, 0x140ad54 (stub 0x1005370)
};

struct CTransfQ {  // 32 bytes: rotation at +0x00, position at +0x10, w at +0x1c
    CQuaternion rotation;
    float position[3];
    float w;
};
static_assert(sizeof(CTransfQ) == 0x20);

struct CRay { C3Vector origin; C3Vector direction; };
struct SBVHHit { std::byte raw[0x48]; };  // total size not resolved; +0x00 and +0x10 are used here
struct SCompactPoolHandle { std::uint32_t raw; };

class CncaGameSpace {
public:
    bool StaticRaycast(const CRay& ray, SBVHHit& hit, CncaGameSpaceMT*& mt) const;  // 0x19066e4 (stub 0xfd1740)
};

class CDeferredMethodQueue {
public:
    // QueueFunctor<SFunctor2<void, CR15NetBotCS, SCompactPoolHandle, CTransfQ>>, 0x13c0c10 (stub 0x1009ed0)
    void* QueueFunctor(void (*invoker)(CDeferredMethodQueue&, char*));
};

class CRandom {
public:
    C3Vector C3Vector_(float radius);  // CRandom::C3Vector_(float), 0x1429308 (stub 0xfe21c0)
};

class CRndSeed {
public:
    void Twister();  // CRndSeed::Twister(), 0x1428b28 (stub 0xfd7bd0)
};

class CSysThread {
public:
    static void* GetTLS(int slot);  // CSysThread::GetTLS(int), 0x108a22c (stub 0x104b720)
};

// Globals read by the function. Their values are not in the file bytes read here.
extern void (*const g_queue_invoker_0x3823a00)(CDeferredMethodQueue&, char*);
extern const void* const g_functor_vptr_0x3819a60;
extern const int g_tls_slot_0x386c350;
extern const C3Vector* const g_up_ptr_0x3814e98;

namespace NRadGame {

struct SR15NetAIContext;
struct SR15NetAgent;

class CR15NetBotCS {
public:
    CTransfQ Transform(SCompactPoolHandle handle) const;                        // 0x2218ee4 (stub 0x103ad30)
    std::uint32_t IsMoving(SCompactPoolHandle handle) const;                    // 0x221dee0 (stub 0xfea710)
    void MoveToTransform(SCompactPoolHandle handle, const CTransfQ& transform); // 0x221dd24 (stub 0x101b360)
};

class SR15PatrolSM {
public:
    void Update(SR15NetAIContext& ctx, SR15NetAgent& agent);
};

void SR15PatrolSM::Update(SR15NetAIContext& ctx, SR15NetAgent& agent) {
    auto* self = reinterpret_cast<std::byte*>(this);
    const auto* c = reinterpret_cast<const std::byte*>(&ctx);
    const auto* a = reinterpret_cast<const std::byte*>(&agent);

    std::uint16_t state = 0;
    std::memcpy(&state, self + 0x08, sizeof state);
    CR15NetBotCS* bot = nullptr;
    std::memcpy(&bot, c + 0x08, sizeof bot);
    SCompactPoolHandle handle{};
    std::memcpy(&handle.raw, a + 0x04, sizeof handle.raw);

    if (state == 1) {
        if (bot->IsMoving(handle) == 0) {
            const std::uint16_t idle = 0;
            std::memcpy(self + 0x08, &idle, sizeof idle);
        }
        return;
    }
    if (state != 0) return;

    std::byte* world = nullptr;
    std::memcpy(&world, c, sizeof world);
    CncaGameSpace* ray_space = nullptr;
    std::memcpy(&ray_space, world + 0x2b08, sizeof ray_space);

    const CTransfQ origin = bot->Transform(handle);
    void* rnd_obj = CSysThread::GetTLS(g_tls_slot_0x386c350);
    const C3Vector raw_dir = static_cast<CRandom*>(rnd_obj)->C3Vector_(3.14159265f);
    float len2 = raw_dir.v[0] * raw_dir.v[0];
    len2 = std::fma(raw_dir.v[1], raw_dir.v[1], len2);
    len2 = std::fma(raw_dir.v[2], raw_dir.v[2], len2);
    const float inv_len = 1.0f / std::sqrt(len2);
    const C3Vector direction{{raw_dir.v[0] * inv_len, raw_dir.v[1] * inv_len, raw_dir.v[2] * inv_len}};

    CRay ray{};
    std::memcpy(ray.origin.v, origin.position, sizeof origin.position);
    ray.direction = direction;
    SBVHHit hit{};
    const std::uint64_t all_ones = ~std::uint64_t{0};
    std::memcpy(hit.raw, &all_ones, sizeof all_ones);
    CncaGameSpaceMT* mt = nullptr;
    const bool raycast_flag = ray_space->StaticRaycast(ray, hit, mt);

    CTransfQ target{};
    if (!raycast_flag) {
        target = origin;
    } else {
        auto* rnd = static_cast<std::byte*>(rnd_obj);
        std::uint32_t index = 0;
        std::memcpy(&index, rnd + 0x1380, sizeof index);
        if (index == 0x270) {
            static_cast<CRndSeed*>(rnd_obj)->Twister();
            std::memcpy(&index, rnd + 0x1380, sizeof index);
        }
        const std::uint32_t next = index + 1;
        std::memcpy(rnd + 0x1380, &next, sizeof next);
        std::uint32_t word = 0;
        std::memcpy(&word, rnd + 0x9c0 + static_cast<std::size_t>(index) * sizeof word, sizeof word);

        const std::uint32_t unit_bits = 0x3f800000u | (word & 0x007fffffu);
        float unit_plus_one = 0.0f;
        std::memcpy(&unit_plus_one, &unit_bits, sizeof unit_plus_one);
        float dist = 0.0f;
        std::memcpy(&dist, hit.raw + 0x10, sizeof dist);
        const float scale = std::fma(unit_plus_one, dist, -dist);

        const C3Vector* up = g_up_ptr_0x3814e98;
        const float ux = up->v[0], uy = up->v[1], uz = up->v[2];
        const float dx = direction.v[0], dy = direction.v[1], dz = direction.v[2];
        const float t_uz_dy = uz * dy;
        const float t_ux_dz = ux * dz;
        const float cx = std::fma(uy, dz, -t_uz_dy);
        const float t_uy_dx = uy * dx;
        const float cy = std::fma(uz, dx, -t_ux_dz);
        const float cz = std::fma(ux, dy, -t_uy_dx);
        float c_len2 = cx * cx;
        c_len2 = std::fma(cy, cy, c_len2);
        c_len2 = std::fma(cz, cz, c_len2);
        const float c_inv = 1.0f / std::sqrt(c_len2);
        const float nx = c_inv * cx;
        const float ny = c_inv * cy;
        const float nz = c_inv * cz;
        const float t_ny_dz = ny * dz;
        const float t_nz_dx = nz * dx;
        const float t_nx_dy = nx * dy;
        const float r0 = std::fma(nz, dy, -t_ny_dz);
        const float r1 = std::fma(nx, dz, -t_nz_dx);
        const float r2 = std::fma(ny, dx, -t_nx_dy);

        const float px = std::fma(scale, dx, origin.position[0]);
        const float py = std::fma(scale, dy, origin.position[1]);
        const float pz = std::fma(scale, dz, origin.position[2]);

        const C33Matrix basis{{nx, ny, nz, r0, r1, r2, dx, dy, dz}};
        CQuaternion rotation{};
        rotation.FromRotationMatrix(basis);
        target.rotation = rotation;
        target.position[0] = px;
        target.position[1] = py;
        target.position[2] = pz;
        target.w = 1.0f;
    }

    std::memcpy(self + 0x10, &target, sizeof target);
    std::byte* gs = nullptr;
    std::memcpy(&gs, reinterpret_cast<const std::byte*>(bot) + 0x80, sizeof gs);
    std::uint32_t queued = 0;
    std::memcpy(&queued, gs + 0x368, sizeof queued);
    if (queued == 0) {
        auto* queue = reinterpret_cast<CDeferredMethodQueue*>(gs + 0x170);
        auto* functor = static_cast<std::byte*>(queue->QueueFunctor(g_queue_invoker_0x3823a00));
        const void* vptr = g_functor_vptr_0x3819a60;
        const std::uint64_t zero = 0;
        std::memcpy(functor + 0x00, &bot, sizeof bot);
        std::memcpy(functor + 0x08, &vptr, sizeof vptr);
        std::memcpy(functor + 0x10, &zero, sizeof zero);
        std::memcpy(functor + 0x18, &handle.raw, sizeof handle.raw);
        std::memcpy(functor + 0x1c, &target, sizeof target);
    } else {
        bot->MoveToTransform(handle, target);
    }
    const std::uint16_t moving = 1;
    std::memcpy(self + 0x08, &moving, sizeof moving);
}

}  // namespace NRadGame
}  // namespace NRadEngine
