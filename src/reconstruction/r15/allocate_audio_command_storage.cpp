#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a53580 — AllocateAudioCommandStorage
/* @confidence: M — stores the rounded byte count and command kind in the allocated block. */
std::byte* AllocateAudioCommandStorage(std::uint64_t context,
                                       std::uint16_t command_kind,
                                       std::uint32_t command_size) {
    auto stored_size = command_size;
    auto* block = AllocateAudioCommandRegion(context, &stored_size);
    if (block != nullptr) {
        *reinterpret_cast<std::uint16_t*>(block) =
            static_cast<std::uint16_t>(stored_size);
        *reinterpret_cast<std::uint16_t*>(block + 2) = command_kind;
    }
    return block;
}

}  // namespace NRadEngine::AudioPost
