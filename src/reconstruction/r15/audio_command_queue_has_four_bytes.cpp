#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a56020 — AudioCommandQueueHasFourBytes
/* @confidence: H — compares the cursor at offset 0x18 with head plus four. */
bool AudioCommandQueueHasFourBytes(const std::uint64_t* queue) {
    return queue[3] < queue[0] + 4U;
}

}  // namespace NRadEngine::AudioPost
