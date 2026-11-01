#include "wwise_post.hpp"

#include <chrono>
#include <thread>

namespace NRadEngine::AudioPost {

// @0x2a4b110 — SleepAudioCommandWait
/* @confidence: M — converts the raw millisecond count to microseconds and waits once. */
void SleepAudioCommandWait(std::uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace NRadEngine::AudioPost
