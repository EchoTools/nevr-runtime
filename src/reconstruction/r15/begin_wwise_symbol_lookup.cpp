// Quest libr15.so VA 0x2a4ab40; ReVault binary 576.
// ReVault raw parent 100335376, reconstruction node 100600386; grade H.
#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2a4ab40 — BeginWwiseSymbolLookup
/* @confidence: H — stores the mutex address in the scope and acquires that mutex. */
void BeginWwiseSymbolLookup(std::byte* scope, std::uintptr_t mutex) {
    *reinterpret_cast<void**>(scope) = reinterpret_cast<void*>(mutex);
    LockAudioMutex(reinterpret_cast<void*>(mutex));
}

}  // namespace NRadEngine::nevr_reconstruction
