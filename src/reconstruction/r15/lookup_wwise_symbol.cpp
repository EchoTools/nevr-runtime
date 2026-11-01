// Quest libr15.so VA 0x2a51d60; ReVault binary 576.
// ReVault raw parent 100335754, reconstruction node 100600410; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a51d60 — LookupWwiseSymbol
/* @confidence: H — reduces the identity hash by bucket count, then searches that chain. */
void* LookupWwiseSymbol(std::uintptr_t table, std::uint32_t event) {
    const auto bucket_count = WwiseSymbolBucketCountThunk(
        reinterpret_cast<const void*>(table));
    if (bucket_count == 0) {
        return nullptr;
    }
    const auto hash = WwiseSymbolHashInput(event);
    const auto bucket_index = hash % bucket_count;
    return FindWwiseSymbolBucketEntry(reinterpret_cast<const void*>(table), event,
                                      bucket_index);
}

}  // namespace NRadEngine::AudioPost
