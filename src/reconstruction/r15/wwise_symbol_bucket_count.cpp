// Quest libr15.so VA 0x2a51e50; ReVault binary 576.
// ReVault raw parent 100335759, reconstruction node 100600408; grade H.
#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2a51e50 — WwiseSymbolBucketCount
/* @confidence: H — loads the 32-bit bucket count at table offset 0x08. */
std::uint32_t WwiseSymbolBucketCount(const void* table) {
    return *reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::byte*>(table) + 8);
}

}  // namespace NRadEngine::nevr_reconstruction
