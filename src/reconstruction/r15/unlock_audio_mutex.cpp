// Quest libr15.so VA 0x2a48e90; ReVault binary 576.
// ReVault raw parent 100335313, reconstruction node 100600388; grade M.
#include "wwise_post.hpp"

#include <cstdint>

extern "C" int pthread_mutex_unlock(void* mutex);

namespace NRadEngine::nevr_reconstruction {

// @0x2a48e90 — UnlockAudioMutex
/* @confidence: M — calls the imported pthread mutex unlock and maps zero/nonzero to 1/2. */
std::uint32_t UnlockAudioMutex(void* mutex) {
    return pthread_mutex_unlock(mutex) == 0 ? 1U : 2U;
}

}  // namespace NRadEngine::nevr_reconstruction
