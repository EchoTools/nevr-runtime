// invoke_method
// Quest libr15.so VA 0x1877ac4; ReVault binary 576.
// ReVault raw parent 100253381, reconstruction node 100600258; grade H: All eight ARM64 instructions support the adjusted-this and optional virtual dispatch.
// @0x1877ac4 — CDeferredMethodQueue::SFunctor1<void, CncaGameSpace, SActor>::InvokeMethod
/* @confidence: H */

#include <cstddef>
#include <cstdint>

namespace NRadEngine {
struct CDeferredMethodQueue;
namespace {
std::uint64_t LoadU64(const void* base, std::size_t offset) {
    return *reinterpret_cast<const std::uint64_t*>(
        static_cast<const std::byte*>(base) + offset);
}
}

void CDeferredMethodQueue_SFunctor1_CncaGameSpace_SActor_InvokeMethod(
    CDeferredMethodQueue* /*queue*/, void* functor) {
    const std::uint64_t encoded_adjustment = LoadU64(functor, 0x10);
    auto* adjusted_this = reinterpret_cast<std::byte*>(
        static_cast<std::uintptr_t>(LoadU64(functor, 0)) +
        (static_cast<std::int64_t>(encoded_adjustment) >> 1));
    std::uintptr_t method = static_cast<std::uintptr_t>(LoadU64(functor, 8));

    if ((encoded_adjustment & 1u) != 0) {
        const std::uintptr_t selector =
            *reinterpret_cast<const std::uintptr_t*>(adjusted_this);
        method = *reinterpret_cast<const std::uintptr_t*>(method + selector);
    }

    using Method = void (*)(void*, std::uint64_t);
    reinterpret_cast<Method>(method)(
        adjusted_this, LoadU64(functor, 0x18));
}
} // namespace NRadEngine
