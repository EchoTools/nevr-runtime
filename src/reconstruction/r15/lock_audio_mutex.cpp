#include "wwise_post.hpp"

#include <cstdint>

extern "C" int pthread_mutex_lock(void* mutex);

namespace NRadEngine::AudioPost {

// @0x2a48e70 — LockAudioMutex
/* @confidence: M — calls the imported pthread mutex lock and maps zero/nonzero to 1/2. */
std::uint32_t LockAudioMutex(void* mutex) {
    return pthread_mutex_lock(mutex) == 0 ? 1U : 2U;
}

}  // namespace NRadEngine::AudioPost
