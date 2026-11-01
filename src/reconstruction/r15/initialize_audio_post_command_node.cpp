#include "wwise_post.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2aad180 — InitializeAudioPostCommandNode
/* @confidence: H — initializes both leading pointer pairs and clears the callback metadata fields. */
void InitializeAudioPostCommandNode(std::byte* node) {
    std::fill_n(node, 0x20, std::byte{});
    *reinterpret_cast<std::uint64_t*>(node + 0x40) = 0;
    *reinterpret_cast<std::uint64_t*>(node + 0x48) = 0;
    InitializeCallbackArgument(node + 0x38, nullptr);
}

}  // namespace NRadEngine::AudioPost
