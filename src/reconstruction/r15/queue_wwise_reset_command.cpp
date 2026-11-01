#include "wwise_post.hpp"

#include <cstring>

namespace NRadEngine::AudioPost {

// @0x2a4a160 — QueueWwiseResetCommand
/* @confidence: H — both reset-command variants, observed sizes, field offsets and scope commit match the complete stored body. */
std::uint64_t QueueWwiseResetCommand(std::uint32_t symbol,
                                     std::uint64_t object_id,
                                     std::uint32_t scaled_value,
                                     std::uint32_t command_kind,
                                     std::uint32_t flags) {
    alignas(8) std::byte scope[8]{};
    const bool basic_command = scaled_value == 0 && (flags & 1U) == 0;
    BeginAudioCommandScope(scope, basic_command ? 5U : 6U,
                           basic_command ? 0x14U : 0x20U);

    auto* command = GetAudioCommandScope(scope);
    const std::uint32_t zero = 0;
    std::memcpy(command + 0x04, &object_id, sizeof(object_id));
    std::memcpy(command + 0x0c, &symbol, sizeof(symbol));
    std::memcpy(command + 0x10, &zero, sizeof(zero));
    if (!basic_command) {
        const auto normalized_flag = flags & 1U;
        std::memcpy(command + 0x14, &scaled_value, sizeof(scaled_value));
        std::memcpy(command + 0x18, &command_kind, sizeof(command_kind));
        std::memcpy(command + 0x1c, &normalized_flag, sizeof(normalized_flag));
    }

    CommitAudioCommandScope(scope);
    return 1;
}

}  // namespace NRadEngine::AudioPost
