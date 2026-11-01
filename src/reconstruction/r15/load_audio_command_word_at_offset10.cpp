#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a4ac50 — LoadAudioCommandWordAtOffset10
/* @confidence: H — reads the 32-bit word at byte offset 0x10. */
std::uint32_t LoadAudioCommandWordAtOffset10(const void* object) {
    return *reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::byte*>(object) + 0x10);
}

}  // namespace NRadEngine::AudioPost
