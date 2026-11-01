// Quest libr15.so VA 0x144c2a8; ReVault binary 576.
// ReVault raw parent 100227800, reconstruction node 100600438; grade H.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x144c2a8 — CSoundPlayer::Play2DSound
/* @confidence: H — forwards the symbol to the current 2D emitter and returns -1 when none is configured. */
std::int32_t Play2DSound(std::uint64_t player, std::uint64_t event,
                         float volume) {
    const auto emitter_context =
        *reinterpret_cast<const std::uintptr_t*>(player + 0x238);
    if (emitter_context == 0) {
        return -1;
    }

    const auto emitter = *reinterpret_cast<const std::uint32_t*>(
        emitter_context + 0xb8);
    return Play3DSound(player, event, emitter, volume);
}

}  // namespace NRadEngine::AudioPost
