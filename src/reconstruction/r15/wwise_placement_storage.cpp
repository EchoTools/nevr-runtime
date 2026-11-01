// Quest libr15.so VA 0x2a52040; ReVault binary 576.
// ReVault raw parent 100335766, reconstruction node 100600425; grade H.
#include "wwise_post.hpp"

#include <cstddef>

namespace NRadEngine::AudioPost {

// @0x2a52040 — WwisePlacementStorage
/* @confidence: H — returns the supplied placement address unchanged. */
void* WwisePlacementStorage(std::size_t size, void* storage) {
    (void)size;
    return storage;
}

}  // namespace NRadEngine::AudioPost
