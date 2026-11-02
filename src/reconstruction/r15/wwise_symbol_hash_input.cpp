// Quest libr15.so VA 0x2a51df0; ReVault binary 576.
// ReVault raw parent 100335757, reconstruction node 100600411; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2a51df0 — WwiseSymbolHashInput
/* @confidence: H — the raw body is a return instruction, preserving the input in w0. */
std::uint32_t WwiseSymbolHashInput(std::uint32_t event) {
    return event;
}

}  // namespace NRadEngine::nevr_reconstruction
