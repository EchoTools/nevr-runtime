// Quest libr15.so VA 0x2a4b110; ReVault binary 576.
// ReVault raw parent 100335400, reconstruction node 100600423; grade M.
#include "wwise_post.hpp"

#include <chrono>
#include <thread>

namespace NRadEngine::nevr_reconstruction {

// @0x2a4b110 — SleepAudioCommandWait
/* @confidence: M — converts the raw millisecond count to microseconds and waits once. */
void SleepAudioCommandWait(std::uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace NRadEngine::nevr_reconstruction
