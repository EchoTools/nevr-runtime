// @0x1988c04 — NRadEngine::CGRawInterfaceVK::DeferredDestroy(SGImage&)
// ReVault binary 576; reconstruction node 100600218.
#include <cstdint>
#include <cstddef>
#include <cstring>

#include "cg_raw_interface_vk.hpp"
#include "cmemory_context.hpp"

namespace NRadEngine {

void FUN_01988a98(std::uint32_t kind, std::uint64_t value);

namespace {

using AllocatorRelease = void (*)(CAllocator*, SGAllocatedBase*);

template <typename T>
T Load(const void* base, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset,
                sizeof(value));
    return value;
}

template <typename T>
void Store(void* base, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof(value));
}

}  // namespace

void CGRawInterfaceVK::DeferredDestroy(SGImage& image) {
    auto* const allocation = image.allocation;
    if (allocation == nullptr) {
        return;
    }

    auto* const metadata = reinterpret_cast<std::byte*>(allocation);
    const auto reference_count = Load<std::int64_t>(metadata, 0x30);
    if (reference_count - 1 == 0) {
        if (Load<std::uint64_t>(metadata, 0) != 0) {
            FUN_01988a98(10, Load<std::uint64_t>(metadata, 0x28));
            DeferredFree(*allocation);
        }

        auto* const previous_allocator = CMemoryContext::Current();
        if (CMemoryContext::context == nullptr) {
            NWriteLog::WriteLog(
                8, 0, "Trying to use memory context before it's initialized!");
            SoftwareBreakpoint(1, 0x1988cb4)();
        }

        auto* const default_allocator = static_cast<CAllocator*>(
            Load<void*>(CMemoryContext::context, 0));
        CMemoryContext::SetCurrent(default_allocator);
        if (CMemoryContext::context == nullptr) {
            NWriteLog::WriteLog(
                8, 0, "Trying to use memory context before it's initialized!");
            SoftwareBreakpoint(1, 0x1988ce0)();
        }

        auto** vtable = *reinterpret_cast<void***>(default_allocator);
        auto release = reinterpret_cast<AllocatorRelease>(
            vtable[0x38 / sizeof(void*)]);
        release(default_allocator, allocation);
        CMemoryContext::SetCurrent(previous_allocator);
    } else {
        Store<std::int64_t>(metadata, 0x30, reference_count - 1);
    }
    image.allocation = nullptr;
}

}  // namespace NRadEngine
