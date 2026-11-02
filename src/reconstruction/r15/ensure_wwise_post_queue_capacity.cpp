// Quest libr15.so VA 0x2aaed80; ReVault binary 576.
// ReVault raw parent 100340322, reconstruction node 100600432; grade M.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2aaed80 — EnsureWwisePostQueueCapacity
/* @confidence: M — preserves the raw 0.9 load threshold and grows the table when empty or overloaded. */
bool EnsureWwisePostQueueCapacity(std::byte* queue) {
    auto bucket_count = *reinterpret_cast<std::uint32_t*>(queue + 8);
    if (bucket_count != 0) {
        const auto entry_count =
            *reinterpret_cast<std::uint32_t*>(queue + 0x10);
        const auto load = static_cast<float>(entry_count) /
                          static_cast<float>(bucket_count);
        if (load <= 0.9F) {
            return true;
        }
    }

    GrowWwisePostQueue(queue, bucket_count);
    bucket_count = *reinterpret_cast<std::uint32_t*>(queue + 8);
    return bucket_count != 0;
}

}  // namespace NRadEngine::AudioPost
