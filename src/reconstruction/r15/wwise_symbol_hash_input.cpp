#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a51df0 — WwiseSymbolHashInput
/* @confidence: H — the raw body is a return instruction, preserving the input in w0. */
std::uint32_t WwiseSymbolHashInput(std::uint32_t event) {
    return event;
}

}  // namespace NRadEngine::AudioPost
