#include "wwise_post.hpp"

#include <cstddef>

namespace NRadEngine::AudioPost {

// @0x2a4b960 — AddAudioCommandReference
/* @confidence: H — stores the supplied reference through the destination and returns it. */
void* AddAudioCommandReference(std::byte* destination, void* reference) {
    *reinterpret_cast<void**>(destination) = reference;
    return reference;
}

}  // namespace NRadEngine::AudioPost
