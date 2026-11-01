#include "wwise_post.hpp"

#include <atomic>

namespace NRadEngine::AudioPost {

// @0x2a534c0 — AudioCommandMemoryBarrier
/* @confidence: M — supplies the full data-memory barrier requested by the raw helper. */
void AudioCommandMemoryBarrier() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

}  // namespace NRadEngine::AudioPost
