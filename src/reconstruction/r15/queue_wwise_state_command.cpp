// Quest libr15.so VA 0x2a4a050; ReVault binary 576.
// ReVault raw parent 100335361, reconstruction node 100600531; grade H.
#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2a4a050 — QueueWwiseStateCommand
/* @confidence: H — forwards both IDs with zero flags and returns the observed success value. */
std::uint64_t QueueWwiseStateCommand(std::uint32_t group,
                                     std::uint32_t state) {
    BuildWwiseStateCommand(group, state, 0, 0);
    return 1;
}

}  // namespace NRadEngine::AudioPost
