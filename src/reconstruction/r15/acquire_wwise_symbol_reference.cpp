#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a51dc0 — AcquireWwiseSymbolReference
/* @confidence: H — increments the 32-bit reference count at symbol offset 0x14. */
std::int32_t AcquireWwiseSymbolReference(void* symbol) {
    auto* count = reinterpret_cast<std::int32_t*>(
        static_cast<std::byte*>(symbol) + 0x14);
    return ++*count;
}

}  // namespace NRadEngine::AudioPost
