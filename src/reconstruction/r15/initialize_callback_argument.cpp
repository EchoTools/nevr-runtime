#include "wwise_post.hpp"

#include <cstddef>

namespace NRadEngine::AudioPost {

// @0x2a4b6b0 — InitializeCallbackArgument
/* @confidence: M — stores the callback argument pointer at the supplied command offset. */
const void* InitializeCallbackArgument(std::byte* destination,
                                      const void* argument) {
    *reinterpret_cast<const void**>(destination) = argument;
    return argument;
}

}  // namespace NRadEngine::AudioPost
