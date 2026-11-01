// Quest libr15.so VA 0x2a490d0; ReVault binary 576.
// ReVault raw parent 100335323, reconstruction node 100600355; grade M.
#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2a490d0 — BeginAudioCommandScope
/* @confidence: M — matches the raw allocator lookup and stored command handle. */
void BeginAudioCommandScope(void* scope, std::uint16_t command_kind,
                            std::uint32_t command_size) {
    *reinterpret_cast<void**>(scope) =
        AllocateAudioCommandStorage(DAT_038cec78, command_kind, command_size);
}

}  // namespace NRadEngine::AudioPost
