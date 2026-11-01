#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a51de0 — WwiseSymbolBucketCountThunk
/* @confidence: H — tail-dispatches to the table bucket-count load. */
std::uint32_t WwiseSymbolBucketCountThunk(const void* table) {
    return WwiseSymbolBucketCount(table);
}

}  // namespace NRadEngine::AudioPost
