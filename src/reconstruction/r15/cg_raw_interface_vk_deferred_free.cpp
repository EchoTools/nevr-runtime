// @0x19888b0 — NRadEngine::CGRawInterfaceVK::DeferredFree(SGAllocatedBase&)
// ReVault binary 576; reconstruction node 100600221.
#include <cstddef>
#include <cstdint>

#include "cg_raw_interface_vk.hpp"

namespace NRadEngine {

void FUN_019ebb48(std::uint32_t milliseconds);

namespace CGS {
extern std::uint64_t currframe;
}  // namespace CGS

namespace CMemBlock {
void Resize(std::uint64_t object, std::size_t bytes);
}  // namespace CMemBlock

struct DeferredEntry {
    std::uint64_t frame;
    SGAllocatedBase allocation;
};

struct DeferredQueue {
    std::byte data_pointer[0x20];
    std::uint64_t minimum_growth;
    std::uint64_t capacity;
    std::uint64_t count;
};

static_assert(sizeof(DeferredEntry) == 0x28);
static_assert(offsetof(DeferredQueue, minimum_growth) == 0x20);
static_assert(offsetof(DeferredQueue, capacity) == 0x28);
static_assert(offsetof(DeferredQueue, count) == 0x30);

// Game globals, defined by the binary. They keep external linkage: declared in the
// anonymous namespace they are used but never defined in this translation unit
// (clang: -Wundefined-internal).
extern DeferredQueue g_deferred_free_queue;  // DAT_0389b2d8
extern std::int32_t g_deferred_free_lock;    // DAT_0389b488
extern std::uint64_t g_resource_frame;       // DAT_03875ff8
extern std::int32_t g_upload_lock;           // DAT_0389b780
extern std::uint64_t g_upload_in_flight;     // DAT_0389b400

namespace {

void Acquire(std::int32_t* lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0) {
    }
}

void Release(std::int32_t* lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

}  // namespace

void CGRawInterfaceVK::DeferredFree(SGAllocatedBase& allocation) {
    if (allocation.memory == 0) {
        return;
    }

    if (CGS::currframe != g_resource_frame) {
        Acquire(&g_deferred_free_lock);

        auto& queue = g_deferred_free_queue;
        if (queue.capacity <= queue.count) {
            auto growth = queue.capacity;
            if (growth <= queue.minimum_growth) {
                growth = queue.minimum_growth;
            }
            CMemBlock::Resize(
                reinterpret_cast<std::uintptr_t>(&queue),
                static_cast<std::size_t>(growth + queue.capacity) *
                    sizeof(DeferredEntry));
            queue.capacity += growth;
        }

        auto* entries = reinterpret_cast<DeferredEntry*>(
            *reinterpret_cast<void**>(&queue));
        entries[queue.count] = {g_resource_frame + 2, allocation};
        ++queue.count;
        Release(&g_deferred_free_lock);
        return;
    }

    Acquire(&g_upload_lock);
    while (g_upload_in_flight != 0) {
        FUN_019ebb48(0x20);
    }
    Release(&g_upload_lock);

    // The reconstructed Free body ignores its size argument.
    Free(reinterpret_cast<VkDeviceMemory_T*>(allocation.memory),
         static_cast<unsigned long>(allocation.begin),
         static_cast<unsigned long>(allocation.size),
         static_cast<unsigned long>(allocation.end));
}

}  // namespace NRadEngine
