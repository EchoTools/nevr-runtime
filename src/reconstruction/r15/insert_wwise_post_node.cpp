// Quest libr15.so VA 0x2aad1a0; ReVault binary 576.
// ReVault raw parent 100340238, reconstruction node 100600433; grade M.
#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2aad1a0 — InsertWwisePostNode
/* @confidence: M — preserves queue growth, callback-count bucket selection, intrusive head insertion and count update. */
bool InsertWwisePostNode(std::uint64_t context, std::byte* node) {
    auto* queue = reinterpret_cast<std::byte*>(context);
    if (!EnsureWwisePostQueueCapacity(queue)) {
        return false;
    }

    const auto callback_count =
        *reinterpret_cast<const std::uint32_t*>(node + 0x4c);
    const auto hash = WwiseSymbolHashInput(callback_count);
    const auto bucket_count =
        *reinterpret_cast<const std::uint32_t*>(queue + 8);
    const auto bucket_index = hash % bucket_count;
    auto** buckets = *reinterpret_cast<void***>(queue);
    auto** bucket = reinterpret_cast<void**>(
        reinterpret_cast<std::byte*>(buckets) + bucket_index * sizeof(void*));

    *reinterpret_cast<void**>(node + 0x68) = *bucket;
    *bucket = node;
    ++*reinterpret_cast<std::uint32_t*>(queue + 0x10);
    return true;
}

}  // namespace NRadEngine::nevr_reconstruction
