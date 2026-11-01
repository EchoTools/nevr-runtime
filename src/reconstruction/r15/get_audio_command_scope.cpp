#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2a490f0 — GetAudioCommandScope
/* @confidence: H — the raw function returns the handle stored in its first argument. */
std::byte* GetAudioCommandScope(const void* scope) {
    return static_cast<std::byte*>(*reinterpret_cast<void* const*>(scope));
}

}  // namespace NRadEngine::AudioPost
