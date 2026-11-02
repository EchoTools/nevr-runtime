// Quest libr15.so VA 0x2a52bc0; ReVault binary 576.
// ReVault raw parent 100335850, reconstruction node 100600553; grade H.
#include "wwise_post.hpp"

namespace NRadEngine::nevr_reconstruction {

// @0x2a52bc0 — WwiseAuxSendCommandBaseSize
/* @confidence: H — the complete two-instruction body returns the constant 0x20. */
std::uint64_t WwiseAuxSendCommandBaseSize() {
    return 0x20;
}

}  // namespace NRadEngine::nevr_reconstruction
