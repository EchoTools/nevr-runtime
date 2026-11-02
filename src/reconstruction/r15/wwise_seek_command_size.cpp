// Quest libr15.so VA 0x2a52ce0; ReVault binary 576.
// ReVault raw parent 100335868, reconstruction node 100600406; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2a52ce0 — WwiseSeekCommandSize
/* @confidence: H — raw body returns the constant 0x24. */
std::uint32_t WwiseSeekCommandSize() {
    return 0x24;
}

}  // namespace NRadEngine::nevr_reconstruction
