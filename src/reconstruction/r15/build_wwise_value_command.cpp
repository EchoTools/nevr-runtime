// Quest libr15.so VA 0x2a49c30; ReVault binary 576.
// ReVault raw parent 100335352, reconstruction node 100600505; grade H.
#include "wwise_post.hpp"

#include <cstring>

namespace NRadEngine::AudioPost {

// @0x2a49c30 — BuildWwiseValueCommand
/* @confidence: H — both command layouts, sizes, field offsets and the final queue commit match the complete stored body. */
std::uint64_t BuildWwiseValueCommand(std::uint32_t command_id,
                                     std::uint64_t game_object, float value,
                                     std::uint32_t reserved,
                                     std::uint32_t scaled_value,
                                     std::uint32_t command_kind,
                                     std::uint32_t flags) {
    alignas(8) std::byte scope[8]{};
    const bool basic_command = scaled_value == 0 && (flags & 1U) == 0;
    BeginAudioCommandScope(scope, basic_command ? 2U : 3U,
                           basic_command ? 0x18U : 0x24U);

    auto* command = GetAudioCommandScope(scope);
    std::memcpy(command + 0x04, &game_object, sizeof(game_object));
    std::memcpy(command + 0x0c, &command_id, sizeof(command_id));
    std::memcpy(command + 0x10, &value, sizeof(value));
    std::memcpy(command + 0x14, &reserved, sizeof(reserved));
    if (!basic_command) {
        const auto normalized_flag = flags & 1U;
        std::memcpy(command + 0x18, &scaled_value, sizeof(scaled_value));
        std::memcpy(command + 0x1c, &command_kind, sizeof(command_kind));
        std::memcpy(command + 0x20, &normalized_flag, sizeof(normalized_flag));
    }

    CommitAudioCommandScope(scope);
    return 1;
}

}  // namespace NRadEngine::AudioPost
