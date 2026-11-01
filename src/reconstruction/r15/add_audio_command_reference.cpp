// Quest libr15.so VA 0x2a4b960; ReVault binary 576.
// ReVault raw parent 100335424, reconstruction node 100600370; grade H.
#include "wwise_post.hpp"

#include <cstddef>

namespace NRadEngine::AudioPost {

// @0x2a4b960 — StoreAudioCommandReference
/* @confidence: H — stores the supplied pointer through the destination and returns it. */
void* StoreAudioCommandReference(std::byte* destination, void* reference) {
    *reinterpret_cast<void**>(destination) = reference;
    return reference;
}

}  // namespace NRadEngine::AudioPost
