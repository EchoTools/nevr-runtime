// Quest libr15.so VA 0x2a4b6c0; ReVault binary 576.
// ReVault raw parent 100335419, reconstruction node 100600403; grade H.
#include "wwise_post.hpp"

#include <cstddef>

namespace NRadEngine::nevr_reconstruction {

// @0x2a4b6c0 — CallbackInfoEqualsValue
/* @confidence: H — compares the pointer-sized field at offset zero with the value. */
bool CallbackInfoEqualsValue(const std::byte* storage, std::uintptr_t value) {
    return *reinterpret_cast<const std::uintptr_t*>(storage) == value;
}

}  // namespace NRadEngine::nevr_reconstruction
