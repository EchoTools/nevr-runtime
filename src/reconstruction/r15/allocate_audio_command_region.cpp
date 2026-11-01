#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a537f0 — AllocateAudioCommandRegion
/* @confidence: M — increments the activity count around a ring allocation and releases it on failure. */
std::byte* AllocateAudioCommandRegion(std::uint64_t context,
                                      std::uint32_t* requested_size) {
    auto* activity_count = reinterpret_cast<std::int32_t*>(
        reinterpret_cast<std::byte*>(context) + 0xac);
    NextWwiseCommandId(activity_count);
    auto* block = AllocateAudioRingBlock(context, requested_size);
    if (block != nullptr) {
        return block;
    }
    DecrementAtomicReference(activity_count);
    return nullptr;
}

}  // namespace NRadEngine::AudioPost
