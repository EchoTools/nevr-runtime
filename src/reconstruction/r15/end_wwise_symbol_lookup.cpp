#include "wwise_post.hpp"

#include <cstddef>

namespace NRadEngine::AudioPost {

// @0x2a4ad00 — EndWwiseSymbolLookup
/* @confidence: H — releases the mutex saved in the lookup scope. */
void EndWwiseSymbolLookup(std::byte* scope) {
    UnlockAudioMutex(*reinterpret_cast<void**>(scope));
}

}  // namespace NRadEngine::AudioPost
