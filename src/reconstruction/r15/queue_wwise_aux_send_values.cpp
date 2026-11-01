// Quest libr15.so VA 0x2a4a2b0; ReVault binary 576.
// ReVault raw parent 100335366, reconstruction node 100600550; grade H.
#include "wwise_post.hpp"

#include <cstring>

namespace NRadEngine::AudioPost {

// @0x2a4a2b0 — QueueWwiseAuxSendValues
/* @confidence: H — command kind, variable command size, object/count fields, payload copy, commit, and success result match the complete 32-instruction body. */
std::uint64_t QueueWwiseAuxSendValues(std::uint32_t object_id,
                                     const void* entries,
                                     std::uint32_t count) {
    alignas(8) std::byte scope[8]{};
    const auto payload_size = static_cast<std::uint32_t>(count << 4);
    const auto command_size =
        payload_size +
        static_cast<std::uint32_t>(WwiseAuxSendCommandBaseSize() & 0xffffU) -
        0x10U;
    BeginAudioCommandScope(scope, 0x13, command_size);
    auto* command = GetAudioCommandScope(scope);
    const std::uint64_t wide_object_id = object_id;
    std::memcpy(command + 0x04, &wide_object_id, sizeof(wide_object_id));
    std::memcpy(command + 0x0c, &count, sizeof(count));
    std::memcpy(command + 0x10, entries, payload_size);
    CommitAudioCommandScope(scope);
    return 1;
}

}  // namespace NRadEngine::AudioPost
