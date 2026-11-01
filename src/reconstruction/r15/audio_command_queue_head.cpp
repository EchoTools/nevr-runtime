// Quest libr15.so VA 0x2a56010; ReVault binary 576.
// ReVault raw parent 100335953, reconstruction node 100600398; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a56010 — AudioCommandQueueHead
/* @confidence: H — returns the queue's first 64-bit cursor. */
std::uint64_t AudioCommandQueueHead(const std::uint64_t* queue) {
    return queue[0];
}

}  // namespace NRadEngine::AudioPost
