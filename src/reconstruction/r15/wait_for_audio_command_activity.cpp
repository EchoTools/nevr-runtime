// Quest libr15.so VA 0x2a53480; ReVault binary 576.
// ReVault raw parent 100335906, reconstruction node 100600420; grade M.
#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a53480 — WaitForAudioCommandActivity
/* @confidence: M — backs off while the activity count stays positive, then issues the raw barrier. */
void WaitForAudioCommandActivity(std::uint64_t context) {
    const auto* activity_count = reinterpret_cast<const std::int32_t*>(
        reinterpret_cast<const std::byte*>(context) + 0xac);
    if (*activity_count > 0) {
        std::uint32_t iteration = 0;
        do {
            SleepAudioCommandWait(iteration >> 7U);
            ++iteration;
        } while (*activity_count > 0);
    }
    AudioCommandMemoryBarrier();
}

}  // namespace NRadEngine::AudioPost
