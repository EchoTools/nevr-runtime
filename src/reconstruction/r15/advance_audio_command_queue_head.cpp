// Quest libr15.so VA 0x2a56040; ReVault binary 576.
// ReVault raw parent 100335956, reconstruction node 100600400; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2a56040 — AdvanceAudioCommandQueueHead
/* @confidence: H — aligns the byte count to four and wraps at the queue boundary. */
void AdvanceAudioCommandQueueHead(std::uint64_t* queue,
                                 std::int32_t byte_count) {
    const auto aligned_size =
        static_cast<std::uint32_t>(static_cast<std::uint32_t>(byte_count) + 3U) & ~3U;
    auto next = queue[0] + aligned_size;
    if (queue[3] <= next) {
        next = queue[2];
    }
    queue[0] = next;
}

}  // namespace NRadEngine::nevr_reconstruction
