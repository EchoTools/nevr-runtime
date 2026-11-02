// Quest libr15.so VA 0x2a56020; ReVault binary 576.
// ReVault raw parent 100335954, reconstruction node 100600399; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2a56020 — AudioCommandQueueHasFourBytes
/* @confidence: H — compares the cursor at offset 0x18 with head plus four. */
bool AudioCommandQueueHasFourBytes(const std::uint64_t* queue) {
    return queue[3] < queue[0] + 4U;
}

}  // namespace NRadEngine::nevr_reconstruction
