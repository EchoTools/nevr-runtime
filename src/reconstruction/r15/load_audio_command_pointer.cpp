#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a4b990 — LoadAudioCommandPointer
/* @confidence: H — returns the pointer stored in the first argument. */
void* LoadAudioCommandPointer(void* const* source) {
    return *source;
}

}  // namespace NRadEngine::AudioPost
