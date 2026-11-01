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
