// Quest libr15.so VA 0x2a51e00; ReVault binary 576.
// ReVault raw parent 100335758, reconstruction node 100600409; grade H.
#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

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

}  // namespace NRadEngine::nevr_reconstruction
