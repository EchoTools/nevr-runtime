#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a52ce0 — WwiseSeekCommandSize
/* @confidence: H — raw body returns the constant 0x24. */
std::uint32_t WwiseSeekCommandSize() {
    return 0x24;
}

}  // namespace NRadEngine::AudioPost
