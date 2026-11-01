#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a4ab40 — BeginWwiseSymbolLookup
/* @confidence: H — stores the mutex address in the scope and acquires that mutex. */
void BeginWwiseSymbolLookup(std::byte* scope, std::uintptr_t mutex) {
    *reinterpret_cast<void**>(scope) = reinterpret_cast<void*>(mutex);
    LockAudioMutex(reinterpret_cast<void*>(mutex));
}

}  // namespace NRadEngine::AudioPost
