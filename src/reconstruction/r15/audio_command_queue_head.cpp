#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a56010 — AudioCommandQueueHead
/* @confidence: H — returns the queue's first 64-bit cursor. */
std::uint64_t AudioCommandQueueHead(const std::uint64_t* queue) {
    return queue[0];
}

}  // namespace NRadEngine::AudioPost
