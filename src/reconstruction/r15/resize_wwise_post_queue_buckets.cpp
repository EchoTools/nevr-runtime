// Quest libr15.so VA 0x2aaeb70; ReVault binary 576.
// ReVault raw parent 100340312, reconstruction node 100600434; grade M.
#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2aaeb70 — ResizeWwisePostQueueBuckets
/* @confidence: M — preserves the raw growth-size arithmetic, pool reallocation, bucket copy and old-buffer release. */
bool ResizeWwisePostQueueBuckets(std::byte* table,
                                 std::uint32_t added_buckets) {
    const auto old_capacity =
        *reinterpret_cast<std::uint32_t*>(table + 0xc);
    const auto new_capacity = old_capacity + added_buckets;
    const auto old_bucket_count =
        *reinterpret_cast<std::uint32_t*>(table + 8);
    auto** old_buckets = *reinterpret_cast<void***>(table);
    auto** new_buckets = static_cast<void**>(ReallocateWwisePoolBlock(
        DAT_03859620, old_buckets,
        static_cast<std::size_t>(new_capacity) * sizeof(void*)));
    if (new_buckets == nullptr) {
        return false;
    }

    if (old_buckets != nullptr && old_buckets != new_buckets) {
        for (std::uint32_t i = 0; i < old_bucket_count; ++i) {
            new_buckets[i] = old_buckets[i];
        }
        FreeWwisePoolBlock(DAT_03859620, old_buckets);
    }

    *reinterpret_cast<void***>(table) = new_buckets;
    *reinterpret_cast<std::uint32_t*>(table + 0xc) = new_capacity;
    return true;
}

}  // namespace NRadEngine::AudioPost
