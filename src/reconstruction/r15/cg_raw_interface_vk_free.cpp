// @0x1985410 — NRadEngine::CGRawInterfaceVK::Free(VkDeviceMemory_T*, unsigned long, unsigned long, unsigned long)
/* @confidence: M */
// ReVault binary 576; reconstruction node 100600207.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CMemoryContext {
    static void* context;
};

namespace NWriteLog {
void WriteLog(int severity, void* context, const char* message, ...);
}

struct VkDeviceMemory_T;
using VkDeviceMemory = VkDeviceMemory_T*;

struct FreeRangeEntry {
    std::uintptr_t memory;
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t metadata_18;
    std::uint64_t metadata_20;
};
static_assert(offsetof(FreeRangeEntry, memory) == 0x00);
static_assert(offsetof(FreeRangeEntry, begin) == 0x08);
static_assert(offsetof(FreeRangeEntry, end) == 0x10);
static_assert(offsetof(FreeRangeEntry, metadata_18) == 0x18);
static_assert(offsetof(FreeRangeEntry, metadata_20) == 0x20);
static_assert(sizeof(FreeRangeEntry) == 0x28);

struct AllocationEntry {
    std::uintptr_t memory;
    std::uint64_t metadata_08;
    std::uint64_t metadata_10;
};
static_assert(offsetof(AllocationEntry, memory) == 0x00);
static_assert(offsetof(AllocationEntry, metadata_08) == 0x08);
static_assert(offsetof(AllocationEntry, metadata_10) == 0x10);
static_assert(sizeof(AllocationEntry) == 0x18);

// These byte regions preserve the observed adjacency: the allocation count is
// at +0x5000, the free-range table starts at +0x5008, its count is at +0xa008,
// and the shared spinlock is at +0xce18.
struct FreeGlobalState {
    std::byte allocation_entries[0x5000];
    std::uint32_t allocation_count;
    std::byte alignment_gap[4];
    std::byte range_entries[0x5000];
    std::uint32_t range_count;
    std::byte lock_gap[0x2e0c];
    std::int32_t lock;
};
static_assert(offsetof(FreeGlobalState, allocation_count) == 0x5000);
static_assert(offsetof(FreeGlobalState, range_entries) == 0x5008);
static_assert(offsetof(FreeGlobalState, range_count) == 0xa008);
static_assert(offsetof(FreeGlobalState, lock) == 0xce18);
static_assert(sizeof(void*) == 8);

extern FreeGlobalState g_cg_raw_vk_free_state;

namespace {

template <typename T>
T read_state(std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, reinterpret_cast<const std::byte*>(&g_cg_raw_vk_free_state) + offset,
                sizeof(value));
    return value;
}

template <typename T>
void write_state(std::size_t offset, const T& value) noexcept {
    std::memcpy(reinterpret_cast<std::byte*>(&g_cg_raw_vk_free_state) + offset, &value,
                sizeof(value));
}

constexpr std::size_t kAllocationEntriesOffset =
    offsetof(FreeGlobalState, allocation_entries);
constexpr std::size_t kAllocationCountOffset =
    offsetof(FreeGlobalState, allocation_count);
constexpr std::size_t kRangeEntriesOffset =
    offsetof(FreeGlobalState, range_entries);
constexpr std::size_t kRangeCountOffset =
    offsetof(FreeGlobalState, range_count);
constexpr std::size_t kLockOffset = offsetof(FreeGlobalState, lock);

AllocationEntry read_allocation(std::uint32_t index) noexcept {
    return read_state<AllocationEntry>(
        kAllocationEntriesOffset + static_cast<std::size_t>(index) * sizeof(AllocationEntry));
}

void write_allocation(std::uint32_t index, const AllocationEntry& entry) noexcept {
    write_state(kAllocationEntriesOffset +
                    static_cast<std::size_t>(index) * sizeof(AllocationEntry),
                entry);
}

FreeRangeEntry read_range(std::uint32_t index) noexcept {
    return read_state<FreeRangeEntry>(
        kRangeEntriesOffset + static_cast<std::size_t>(index) * sizeof(FreeRangeEntry));
}

void write_range(std::uint32_t index, const FreeRangeEntry& entry) noexcept {
    write_state(kRangeEntriesOffset +
                    static_cast<std::size_t>(index) * sizeof(FreeRangeEntry),
                entry);
}

void acquire_free_lock() noexcept {
    auto* lock = reinterpret_cast<std::int32_t*>(
        reinterpret_cast<std::byte*>(&g_cg_raw_vk_free_state) + kLockOffset);
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0) {
    }
}

void release_free_lock() noexcept {
    auto* lock = reinterpret_cast<std::int32_t*>(
        reinterpret_cast<std::byte*>(&g_cg_raw_vk_free_state) + kLockOffset);
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

void remove_range(std::uint32_t index) noexcept {
    auto count = read_state<std::uint32_t>(kRangeCountOffset);
    for (std::uint32_t next = index; next + 1 < count; ++next) {
        write_range(next, read_range(next + 1));
    }
    write_state(kRangeCountOffset, count - 1);
}

void remove_allocation(std::uint32_t index) noexcept {
    auto count = read_state<std::uint32_t>(kAllocationCountOffset);
    for (std::uint32_t next = index; next + 1 < count; ++next) {
        write_allocation(next, read_allocation(next + 1));
    }
    write_state(kAllocationCountOffset, count - 1);
}

struct SourceLocation {
    const char* file;
    std::uint64_t line;
};

using AllocationCallback = void* (*)(void*, std::size_t, std::size_t, std::uint32_t);
using ReallocationCallback =
    void* (*)(void*, void*, std::size_t, std::size_t, std::uint32_t);
using FreeCallback = void (*)(void*, void*);
using InternalAllocationCallback =
    void (*)(void*, std::size_t, std::uint32_t, std::uint32_t);

struct VkAllocationCallbacks {
    void* user_data;
    AllocationCallback allocation;
    ReallocationCallback reallocation;
    FreeCallback free;
    InternalAllocationCallback internal_allocation;
    InternalAllocationCallback internal_free;
};
static_assert(sizeof(VkAllocationCallbacks) == 0x30);

[[noreturn]] void memory_context_uninitialized() {
    NWriteLog::WriteLog(
        8, nullptr, "Trying to use memory context before it's initialized!");
    __builtin_trap();
}

void* memory_context_allocator() {
    if (CMemoryContext::context == nullptr) {
        memory_context_uninitialized();
    }

    void* allocator = nullptr;
    std::memcpy(
        &allocator,
        static_cast<const std::byte*>(CMemoryContext::context) + 8,
        sizeof(allocator));
    return allocator;
}

template <typename Function>
Function memory_context_method(void* allocator, std::size_t byte_offset) {
    void** vtable = nullptr;
    std::memcpy(&vtable, allocator, sizeof(vtable));

    Function method = nullptr;
    std::memcpy(
        &method,
        reinterpret_cast<const std::byte*>(vtable) + byte_offset,
        sizeof(method));
    return method;
}

void* R15MemoryContextAllocationCallback(
    void*, std::size_t size, std::size_t alignment, std::uint32_t) {
    void* allocator = memory_context_allocator();
    using Method = void* (*)(void*, std::size_t, std::size_t);
    return memory_context_method<Method>(allocator, 0x30)(
        allocator, alignment, size);
}

void* R15MemoryContextReallocationCallback(
    void*, void* original, std::size_t size, std::size_t, std::uint32_t) {
    void* allocator = memory_context_allocator();
    using Method = void* (*)(void*, void*, std::size_t);
    return memory_context_method<Method>(allocator, 0x18)(
        allocator, original, size);
}

void R15MemoryContextFreeCallback(void*, void* memory) {
    void* allocator = memory_context_allocator();
    using Method = void (*)(void*, void*);
    memory_context_method<Method>(allocator, 0x40)(allocator, memory);
}

void R15MemoryContextInternalAllocationCallback(
    void*, std::size_t, std::uint32_t, std::uint32_t) {}

void R15MemoryContextInternalFreeCallback(
    void*, std::size_t, std::uint32_t, std::uint32_t) {}

struct VulkanDeviceState {
    std::byte unknown_0000[0x108];
    void (*free_memory)(void*, VkDeviceMemory, const VkAllocationCallbacks*);
    std::byte unknown_0110[0xa40];
    void* device;
};
static_assert(offsetof(VulkanDeviceState, free_memory) == 0x108);
static_assert(offsetof(VulkanDeviceState, device) == 0xb50);

extern VulkanDeviceState* g_cg_raw_vk_device_state;

void release_device_memory(VkDeviceMemory memory) {
    thread_local SourceLocation source_location{};
    thread_local VkAllocationCallbacks callbacks{};

    source_location.file =
        "d:\\projects\\rad\\dev\\src\\engine\\libs\\gs\\cgrawinterface_vk.cpp";
    source_location.line = 0xc36;
    callbacks = {
        &source_location,
        R15MemoryContextAllocationCallback,
        R15MemoryContextReallocationCallback,
        R15MemoryContextFreeCallback,
        R15MemoryContextInternalAllocationCallback,
        R15MemoryContextInternalFreeCallback,
    };

    const auto* device_state = g_cg_raw_vk_device_state;
    device_state->free_memory(device_state->device, memory, &callbacks);
}

}  // namespace

class CGRawInterfaceVK {
public:
    static void Free(VkDeviceMemory memory, unsigned long begin,
                     unsigned long size, unsigned long end);
};

void CGRawInterfaceVK::Free(VkDeviceMemory memory, unsigned long begin,
                            unsigned long size, unsigned long end) {
    (void)size;
    const auto memory_key = reinterpret_cast<std::uintptr_t>(memory);

    acquire_free_lock();
    const auto count = read_state<std::uint32_t>(kRangeCountOffset);
    if (count != 0) {
        std::uint32_t current_index = count;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto entry = read_range(index);
            if (entry.memory != memory_key) {
                continue;
            }

            current_index = index;
            if (entry.begin == end) {
                auto enlarged = entry;
                enlarged.begin = begin;
                write_range(index, enlarged);
            } else if (entry.end == begin) {
                auto enlarged = entry;
                enlarged.end = end;
                write_range(index, enlarged);
            } else {
                for (std::uint32_t source = 0; source < count; ++source) {
                    const auto existing = read_range(source);
                    if (existing.memory != memory_key) {
                        continue;
                    }

                    FreeRangeEntry appended{
                        memory_key, begin, end, existing.metadata_18, existing.metadata_20};
                    write_state(kRangeCountOffset, count + 1);
                    write_range(count, appended);
                    release_free_lock();
                    return;
                }
            }
            break;
        }

        if (current_index != count) {
            std::uint32_t candidate = 0;
            while (candidate < read_state<std::uint32_t>(kRangeCountOffset)) {
                const auto current = read_range(current_index);
                const auto adjacent = read_range(candidate);
                if (adjacent.memory != memory_key) {
                    ++candidate;
                    continue;
                }

                if (current.begin == adjacent.end) {
                    auto merged = current;
                    merged.begin = adjacent.begin;
                    write_range(current_index, merged);
                    remove_range(candidate);
                    if (candidate < current_index) {
                        --current_index;
                    }
                    continue;
                }

                if (current.end == adjacent.begin) {
                    auto merged = current;
                    merged.end = adjacent.end;
                    write_range(current_index, merged);
                    remove_range(candidate);
                    if (candidate < current_index) {
                        --current_index;
                    }
                    continue;
                }

                ++candidate;
            }

            release_free_lock();
            return;
        }

        for (std::uint32_t index = 0; index < count; ++index) {
            const auto existing = read_range(index);
            if (existing.memory != memory_key) {
                continue;
            }

            FreeRangeEntry appended{
                memory_key, begin, end, existing.metadata_18, existing.metadata_20};
            write_state(kRangeCountOffset, count + 1);
            write_range(count, appended);
            release_free_lock();
            return;
        }
    }

    release_free_lock();

    const auto allocation_count =
        read_state<std::uint32_t>(kAllocationCountOffset);
    for (std::uint32_t index = 0; index < allocation_count; ++index) {
        if (read_allocation(index).memory == memory_key) {
            remove_allocation(index);
            break;
        }
    }

    release_device_memory(memory);
}

}  // namespace NRadEngine
