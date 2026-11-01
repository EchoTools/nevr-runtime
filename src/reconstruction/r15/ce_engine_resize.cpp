// @0x2bc093c — NRadEngine::CEngine::Resize(unsigned int, unsigned int, EWindowMode)
// ReVault binary 576; reconstruction node 100600216 (raw parent).
#include <cstdint>
#include <cstring>

#include "ce_engine.hpp"
#include "cmemory_context.hpp"

namespace NRadEngine {

namespace {

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

using ContextUpdate = void (*)(void*);

}  // namespace

int CEngine::Resize(std::uint32_t width, std::uint32_t height,
                    std::uint32_t mode) {
    if ((Load<std::uint8_t>(this, 0x16ed4) & 1U) == 0) {
        return 0;
    }

    if (CMemoryContext::context == nullptr) {
        NWriteLog::WriteLog(
            8, 0, "Trying to use memory context before it's initialized!");
        const auto breakpoint = SoftwareBreakpoint(1, 0x2bc0998);
        breakpoint();
    }

    auto* const default_allocator = static_cast<CAllocator*>(
        Load<void*>(CMemoryContext::context, 0));
    auto* const previous_allocator = CMemoryContext::CurrentPtr();
    CMemoryContext::SetCurrent(default_allocator);

    auto* canvas = reinterpret_cast<CCanvasDisplay*>(
        reinterpret_cast<std::byte*>(this) + sizeof(void*));
    const int result = canvas->Resize(&width, &height, mode);
    if (result == 0) {
        const auto render_context = Load<void*>(this, 0x16d60);
        if (render_context != nullptr) {
            const auto viewport_width = Load<std::int32_t>(this, 0x13a60);
            const auto viewport_height = Load<std::int32_t>(this, 0x13a64);

            Store<float>(render_context, 0xa0,
                         static_cast<float>(viewport_width));
            Store<float>(render_context, 0xa4,
                         static_cast<float>(viewport_height));

            auto** vtable = *reinterpret_cast<void***>(render_context);
            auto update = reinterpret_cast<ContextUpdate>(
                vtable[0xd0 / sizeof(void*)]);
            update(render_context);

            Store<float>(render_context, 0xa8,
                         static_cast<float>(viewport_width));
            Store<float>(render_context, 0xac,
                         static_cast<float>(viewport_height));
        }
        Store<std::uint32_t>(this, 0x16dd4, mode);
    }

    CMemoryContext::SetCurrent(previous_allocator);
    return result;
}

}  // namespace NRadEngine
