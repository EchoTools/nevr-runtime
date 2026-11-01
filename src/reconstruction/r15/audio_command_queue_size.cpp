#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a55fd0 — AudioCommandQueueSize
/* @confidence: H — computes used ring-buffer bytes with wrap at the stored capacity. */
std::uint64_t AudioCommandQueueSize(const std::uint64_t* queue) {
    const auto head = queue[0];
    const auto tail = queue[1];
    if (tail < head) {
        return static_cast<std::uint32_t>(queue[4]) - head + tail;
    }
    return tail - head;
}

}  // namespace NRadEngine::AudioPost
