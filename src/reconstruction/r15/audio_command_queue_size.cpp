// Quest libr15.so VA 0x2a55fd0; ReVault binary 576.
// ReVault raw parent 100335952, reconstruction node 100600397; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

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

}  // namespace NRadEngine::nevr_reconstruction
