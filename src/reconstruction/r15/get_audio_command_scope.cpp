// Quest libr15.so VA 0x2a490f0; ReVault binary 576.
// ReVault raw parent 100335324, reconstruction node 100600356; grade H.
#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2a490f0 — GetAudioCommandScope
/* @confidence: H — the raw function returns the handle stored in its first argument. */
std::byte* GetAudioCommandScope(const void* scope) {
    return static_cast<std::byte*>(*reinterpret_cast<void* const*>(scope));
}

}  // namespace NRadEngine::AudioPost
