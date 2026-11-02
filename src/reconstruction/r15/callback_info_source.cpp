// Quest libr15.so VA 0x2a4b880; ReVault binary 576.
// ReVault raw parent 100335421, reconstruction node 100600402; grade H.
#include "wwise_post.hpp"

#include <cstddef>

namespace NRadEngine::AudioPost {

// @0x2a4b880 — CallbackInfoSource
/* @confidence: H — loads the pointer-sized source stored at offset zero. */
void* CallbackInfoSource(const std::byte* storage) {
    return *reinterpret_cast<void* const*>(storage);
}

}  // namespace NRadEngine::AudioPost
