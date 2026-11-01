#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a550b0 — InitializeWwiseCallbackMetadata
/* @confidence: M — copies the raw fields and transfers the intrusive source reference. */
void InitializeWwiseCallbackMetadata(std::byte* destination,
                                     std::uint32_t callback_count,
                                     const std::byte* source) {
    *reinterpret_cast<std::uint32_t*>(destination + 0x14) = callback_count;
    *reinterpret_cast<std::uint64_t*>(destination + 8) =
        *reinterpret_cast<const std::uint64_t*>(source + 8);
    *reinterpret_cast<std::uint32_t*>(destination + 0x10) =
        *reinterpret_cast<const std::uint32_t*>(source + 0x10);

    auto* callback_source = CallbackInfoSource(source);
    if (auto* previous_source = CallbackInfoSource(destination);
        previous_source != nullptr) {
        ReleaseCallbackSource(
            reinterpret_cast<std::uintptr_t>(previous_source));
    }

    if (callback_source != nullptr) {
        ++*reinterpret_cast<std::int32_t*>(callback_source);
    }
    InitializeCallbackArgument(destination, callback_source);
}

}  // namespace NRadEngine::AudioPost
