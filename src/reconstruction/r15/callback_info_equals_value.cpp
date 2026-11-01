#include "wwise_post.hpp"

#include <cstddef>

namespace NRadEngine::AudioPost {

// @0x2a4b6c0 — CallbackInfoEqualsValue
/* @confidence: H — compares the pointer-sized field at offset zero with the value. */
bool CallbackInfoEqualsValue(const std::byte* storage, std::uintptr_t value) {
    return *reinterpret_cast<const std::uintptr_t*>(storage) == value;
}

}  // namespace NRadEngine::AudioPost
