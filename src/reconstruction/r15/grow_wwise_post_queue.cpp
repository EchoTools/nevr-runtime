// Quest libr15.so VA 0x2aaee00; ReVault binary 576.
// ReVault raw parent 100340324, reconstruction node 100600435; grade M.
#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2aaee00 — GrowWwisePostQueue
/* @confidence: M — preserves the raw prime-capacity sequence, bucket rehash and allocation-failure result. */
std::uint32_t GrowWwisePostQueue(std::byte* queue,
                                 std::uint32_t current_bucket_count) {
    constexpr std::uint32_t kBucketLimits[] = {
        0x1d, 0x35, 0x61, 0xc1, 0x185, 0x301, 0x607, 0xc07, 0x1807,
        0x3001, 0x6011, 0xc005, 0x1800d, 0x30005, 0x60019, 0xc0001,
        0x180005, 0x30000b, 0x60000d, 0xc00005, 0x1800013, 0x3000005,
        0x6000017, 0xc000013, 0x18000005, 0x30000059, 0x60000005};
    std::uint32_t new_bucket_count = 0;
    for (const auto limit : kBucketLimits) {
        if (current_bucket_count < limit) {
            new_bucket_count = limit;
            break;
        }
    }
    if (new_bucket_count == 0) {
        return 2;
    }

    auto** old_buckets = *reinterpret_cast<void***>(queue);
    const auto old_bucket_count =
        *reinterpret_cast<std::uint32_t*>(queue + 8);
    const auto old_capacity =
        *reinterpret_cast<std::uint32_t*>(queue + 0xc);
    *reinterpret_cast<void***>(queue) = nullptr;
    *reinterpret_cast<std::uint32_t*>(queue + 8) = 0;
    *reinterpret_cast<std::uint32_t*>(queue + 0xc) = 0;

    if (!ResizeWwisePostQueueBuckets(queue, new_bucket_count)) {
        *reinterpret_cast<void***>(queue) = old_buckets;
        *reinterpret_cast<std::uint32_t*>(queue + 8) = old_bucket_count;
        *reinterpret_cast<std::uint32_t*>(queue + 0xc) = old_capacity;
        return 2;
    }

    auto** buckets = *reinterpret_cast<void***>(queue);
    for (std::uint32_t i = 0; i < new_bucket_count; ++i) {
        buckets[i] = nullptr;
        ++*reinterpret_cast<std::uint32_t*>(queue + 8);
    }

    for (std::uint32_t i = 0; i < old_bucket_count; ++i) {
        auto* node = static_cast<std::byte*>(old_buckets[i]);
        while (node != nullptr) {
            auto* next = *reinterpret_cast<std::byte**>(node + 0x68);
            const auto callback_count =
                *reinterpret_cast<const std::uint32_t*>(node + 0x4c);
            const auto hash = WwiseSymbolHashInput(callback_count);
            const auto bucket_index = hash % new_bucket_count;
            auto** bucket = reinterpret_cast<void**>(
                reinterpret_cast<std::byte*>(buckets) +
                static_cast<std::size_t>(bucket_index) * sizeof(void*));
            *reinterpret_cast<void**>(node + 0x68) = *bucket;
            *bucket = node;
            node = next;
        }
    }

    if (old_buckets != nullptr) {
        FreeWwisePoolBlock(DAT_03859620, old_buckets);
    }
    return 1;
}

}  // namespace NRadEngine::nevr_reconstruction
