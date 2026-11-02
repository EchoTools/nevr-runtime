// Quest libr15.so VA 0x2a51de0; ReVault binary 576.
// ReVault raw parent 100335756, reconstruction node 100600407; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2a51de0 — WwiseSymbolBucketCountThunk
/* @confidence: H — tail-dispatches to the table bucket-count load. */
std::uint32_t WwiseSymbolBucketCountThunk(const void* table) {
    return WwiseSymbolBucketCount(table);
}

}  // namespace NRadEngine::nevr_reconstruction
