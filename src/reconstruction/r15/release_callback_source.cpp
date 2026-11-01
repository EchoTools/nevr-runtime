#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace NRadEngine::AudioPost {

// @0x2a4b890 — ReleaseCallbackSource
/* @confidence: M — decrements the source count and frees copied strings at zero; allocator boundary is inferred. */
void ReleaseCallbackSource(std::uint64_t source_address) {
    auto* source = reinterpret_cast<std::byte*>(source_address);
    auto* reference_count = reinterpret_cast<std::int32_t*>(source);
    const auto remaining = --*reference_count;
    if (remaining != 0) {
        return;
    }

    const auto count = *reinterpret_cast<const std::uint32_t*>(source + 4);
    auto* records = reinterpret_cast<std::uint64_t*>(source + 8);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::free(reinterpret_cast<void*>(records[i * 4U + 1U]));
    }
    std::free(source);
}

}  // namespace NRadEngine::AudioPost
