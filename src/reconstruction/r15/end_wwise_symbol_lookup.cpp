// Quest libr15.so VA 0x2a4ad00; ReVault binary 576.
// ReVault raw parent 100335385, reconstruction node 100600387; grade H.
#include "wwise_post.hpp"

#include <cstddef>

namespace NRadEngine::AudioPost {

// @0x2a4ad00 — EndWwiseSymbolLookup
/* @confidence: H — releases the mutex saved in the lookup scope. */
void EndWwiseSymbolLookup(std::byte* scope) {
    UnlockAudioMutex(*reinterpret_cast<void**>(scope));
}

}  // namespace NRadEngine::AudioPost
