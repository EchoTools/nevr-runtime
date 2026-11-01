#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a51e00 — FindWwiseSymbolBucketEntry
/* @confidence: H — follows bucket head and next pointers until the event id matches. */
void* FindWwiseSymbolBucketEntry(const void* table, std::uint32_t event,
                                 std::uint32_t bucket_index) {
    auto** buckets = *reinterpret_cast<void***>(const_cast<void*>(table));
    auto* entry = static_cast<std::byte*>(buckets[bucket_index]);
    while (entry != nullptr &&
           *reinterpret_cast<const std::uint32_t*>(entry + 0x10) != event) {
        entry = static_cast<std::byte*>(*reinterpret_cast<void**>(entry + 8));
    }
    return entry;
}

}  // namespace NRadEngine::AudioPost
