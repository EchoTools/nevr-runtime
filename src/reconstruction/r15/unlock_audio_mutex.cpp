#include "wwise_post.hpp"

#include <cstdint>

extern "C" int pthread_mutex_unlock(void* mutex);

namespace NRadEngine::AudioPost {

// @0x2a48e90 — UnlockAudioMutex
/* @confidence: M — calls the imported pthread mutex unlock and maps zero/nonzero to 1/2. */
std::uint32_t UnlockAudioMutex(void* mutex) {
    return pthread_mutex_unlock(mutex) == 0 ? 1U : 2U;
}

}  // namespace NRadEngine::AudioPost
