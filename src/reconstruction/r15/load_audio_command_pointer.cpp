// Quest libr15.so VA 0x2a4b990; ReVault binary 576.
// ReVault raw parent 100335426, reconstruction node 100600372; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a4b990 — LoadAudioCommandPointer
/* @confidence: H — returns the pointer stored in the first argument. */
void* LoadAudioCommandPointer(void* const* source) {
    return *source;
}

}  // namespace NRadEngine::AudioPost
