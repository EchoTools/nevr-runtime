// Quest libr15.so VA 0x2a52af0; ReVault binary 576.
// ReVault raw parent 100335837, reconstruction node 100600373; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a52af0 — WwisePostCommandSize
/* @confidence: H — the raw body returns the constant 0x34. */
std::uint32_t WwisePostCommandSize() {
    return 0x34;
}

}  // namespace NRadEngine::AudioPost
