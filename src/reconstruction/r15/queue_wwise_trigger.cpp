// Quest libr15.so VA 0x2a49fc0; ReVault binary 576.
// ReVault raw parent 100335359, reconstruction node 100600542; grade H.
#include "wwise_post.hpp"

#include <cstring>

namespace NRadEngine::AudioPost {

// @0x2a49fc0 — QueueWwiseTrigger
/* @confidence: H — kind 10, 0x10-byte allocation, object and trigger field offsets, commit, and success result match the complete body. */
std::uint64_t QueueWwiseTrigger(std::uint32_t trigger,
                                std::uint32_t object_id) {
    alignas(8) std::byte scope[8]{};
    BeginAudioCommandScope(scope, 10, 0x10);
    auto* command = GetAudioCommandScope(scope);
    const std::uint64_t wide_object_id = object_id;
    std::memcpy(command + 0x04, &wide_object_id, sizeof(wide_object_id));
    std::memcpy(command + 0x0c, &trigger, sizeof(trigger));
    CommitAudioCommandScope(scope);
    return 1;
}

}  // namespace NRadEngine::AudioPost
