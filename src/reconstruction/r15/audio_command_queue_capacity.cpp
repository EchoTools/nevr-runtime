#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a513b0 — AudioCommandQueueCapacity
/* @confidence: H — returns the 32-bit value at queue offset 0x20. */
std::uint32_t AudioCommandQueueCapacity(const void* queue) {
    return *reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::byte*>(queue) + 0x20);
}

}  // namespace NRadEngine::AudioPost
