// Quest libr15.so VA 0x2a49f00; ReVault binary 576.
// ReVault raw parent 100335357, reconstruction node 100600526; grade H.
#include "wwise_post.hpp"

#include <cstring>

namespace NRadEngine::nevr_reconstruction {

// @0x2a49f00 — QueueWwiseSwitchCommand
/* @confidence: H — command kind, size, field offsets, commit and success result match the complete ARM64 body. */
std::uint64_t QueueWwiseSwitchCommand(std::uint32_t group,
                                     std::uint32_t value,
                                     std::uint32_t emitter_id) {
    alignas(8) std::byte scope[8]{};
    BeginAudioCommandScope(scope, 8, 0x14);
    auto* command = GetAudioCommandScope(scope);
    std::memcpy(command + 0x04, &emitter_id, sizeof(emitter_id));
    std::memcpy(command + 0x0c, &group, sizeof(group));
    std::memcpy(command + 0x10, &value, sizeof(value));
    CommitAudioCommandScope(scope);
    return 1;
}

}  // namespace NRadEngine::nevr_reconstruction
