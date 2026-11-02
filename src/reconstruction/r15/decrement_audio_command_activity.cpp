// Quest libr15.so VA 0x2a498c0; ReVault binary 576.
// ReVault raw parent 100335335, reconstruction node 100600391; grade H.
#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2a498c0 — DecrementAudioCommandActivity
/* @confidence: H — decrements the 32-bit counter at context offset 0xac. */
void DecrementAudioCommandActivity(std::uint64_t context) {
    auto* counter = reinterpret_cast<std::int32_t*>(
        reinterpret_cast<std::byte*>(context) + 0xac);
    DecrementAtomicReference(counter);
}

}  // namespace NRadEngine::nevr_reconstruction
