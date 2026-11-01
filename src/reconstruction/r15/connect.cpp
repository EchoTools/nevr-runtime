// CR15NetGunCS::Connect
// Quest libr15.so VA 0x2253438; ReVault binary 576.
// ReVault raw parent 100303097, reconstruction node 100600252; grade M: ARM64 confirms the 64-bit user ID and deferred functor layout; TLS-key value remains uncertain.
// @0x2253438 — CR15NetGunCS::Connect
/* @confidence: M */

#include <cstddef>
#include <cstdint>

namespace NRadEngine {
struct CStackAllocator {
    static void* DirectAlloc(void* tls_allocator, std::size_t bytes,
                             std::size_t alignment);
    void DirectFree();
};
struct CSysThread {
    static void* GetTLS(int index);
};
struct SActor {
    std::uint64_t value;
};
struct CncaGameSpace {
    void EnableActor(SActor actor);
};
void CncaGameSpace_EnableActor(CncaGameSpace* game_space, SActor actor);
struct CDeferredMethodQueue {
    struct SFunctor1 {
        std::byte storage[0x20];
    };
    using InvokeMethodFn = void (*)(CDeferredMethodQueue*, void*);
    static SFunctor1* QueueFunctor(void* queue, InvokeMethodFn invoke_method);
};
void CDeferredMethodQueue_SFunctor1_CncaGameSpace_SActor_InvokeMethod(
    CDeferredMethodQueue* queue, void* functor);

namespace NRadGame {
class CR15NetGunCS {
public:
    void Connect(std::uint64_t user_id);
    void ConnectGun(std::uint64_t user_id, std::uint64_t compact_gun_handle);
};
}

/* Static bytes at 0x38611b8 conflict with pthread-key use; the original symbol value is not identified. */
extern std::uint32_t g_allocator_tls_index;

namespace {
std::uint16_t LoadU16(const void* base, std::size_t offset) {
    return *reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::byte*>(base) + offset);
}
std::uint32_t LoadU32(const void* base, std::size_t offset) {
    return *reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::byte*>(base) + offset);
}
std::uint64_t LoadU64(const void* base, std::size_t offset) {
    return *reinterpret_cast<const std::uint64_t*>(
        static_cast<const std::byte*>(base) + offset);
}
void StoreU64(void* base, std::size_t offset, std::uint64_t value) {
    *reinterpret_cast<std::uint64_t*>(static_cast<std::byte*>(base) + offset) = value;
}
} // namespace

namespace NRadGame {
void CR15NetGunCS::Connect(std::uint64_t user_id) {
    auto* const gun_system = this;
    auto* const self = reinterpret_cast<std::byte*>(this);
    const std::uint16_t initial_gun_count = LoadU16(self, 0xf2);
    if (initial_gun_count == 0) {
        return;
    }

    void* const tls_allocator =
        CSysThread::GetTLS(static_cast<int>(g_allocator_tls_index));
    auto* actor_handles = static_cast<std::uint64_t*>(
        CStackAllocator::DirectAlloc(
            tls_allocator, static_cast<std::size_t>(initial_gun_count) * 8, 8));

    const std::uint16_t current_count = LoadU16(self, 0xf2);
    if (current_count != 0) {
        auto* const gun_slot_map = reinterpret_cast<const std::byte*>(
            LoadU64(self, 0xf8));
        auto* const actor_handle_source = reinterpret_cast<const std::byte*>(
            LoadU64(self, 0x100));
        const auto* const gun_records = reinterpret_cast<const std::byte*>(
            LoadU64(self, 0xb8));
        const std::uint32_t user_slot = user_id & 0xffffu;
        std::uint16_t matched_count = 0;

        for (std::uint64_t index = 0; index < current_count; ++index) {
            const std::uint16_t gun_record_index =
                *reinterpret_cast<const std::uint16_t*>(
                    gun_slot_map + 0xa0 + index * 0x168);
            const std::byte* const gun_record =
                reinterpret_cast<const std::byte*>(
                    LoadU64(gun_records, 0)) +
                static_cast<std::size_t>(gun_record_index) * 0xa0;

            if (LoadU64(gun_record, 0x20) != user_slot ||
                LoadU32(gun_record, 0x9c) == 0) {
                continue;
            }

            const std::uint16_t gun_slot = LoadU16(
                reinterpret_cast<const void*>(LoadU64(self, 0xd8)),
                static_cast<std::size_t>(index) * 2);
            const auto* const gun_slot_metadata =
                reinterpret_cast<const std::byte*>(LoadU64(self, 0xc8)) +
                static_cast<std::size_t>(gun_slot) * 4;
            const std::uint32_t packed_gun_handle =
                static_cast<std::uint32_t>(gun_slot) |
                (static_cast<std::uint32_t>(
                     *reinterpret_cast<const std::uint16_t*>(
                         gun_slot_metadata + 2))
                 << 16);

            gun_system->ConnectGun(user_id, packed_gun_handle);
            actor_handles[matched_count++] = LoadU64(
                actor_handle_source, static_cast<std::size_t>(index) * 0x10);
        }

        if (matched_count != 0) {
            auto* const game_space =
                reinterpret_cast<CncaGameSpace*>(LoadU64(self, 0x80));
            for (std::uint16_t i = 0; i < matched_count; ++i) {
                const SActor actor{actor_handles[i]};
                if (LoadU32(game_space, 0x368) != 0) {
                    game_space->EnableActor(actor);
                } else {
                    auto* const functor = CDeferredMethodQueue::QueueFunctor(
                        reinterpret_cast<std::byte*>(game_space) + 0x170,
                        &CDeferredMethodQueue_SFunctor1_CncaGameSpace_SActor_InvokeMethod);
                    StoreU64(functor, 0,
                             reinterpret_cast<std::uintptr_t>(game_space));
                    StoreU64(functor, 8,
                             reinterpret_cast<std::uintptr_t>(&CncaGameSpace_EnableActor));
                    StoreU64(functor, 0x10, 0);
                    StoreU64(functor, 0x18, actor.value);
                }
            }
        }
    }

    if (tls_allocator != nullptr) {
        static_cast<CStackAllocator*>(tls_allocator)->DirectFree();
    }
}
} // namespace NRadGame
} // namespace NRadEngine
