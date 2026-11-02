// Quest libr15.so VA 0x2a4a070; ReVault binary 576.
// ReVault raw parent 100335362, reconstruction node 100600530; grade H.
#include "wwise_post.hpp"

#include <cstring>

namespace NRadEngine::nevr_reconstruction {

// @0x2a4a070 — BuildWwiseStateCommand
/* @confidence: H — the 0x10-byte kind-7 command, sentinel normalization, flag masks, field offsets and commit match the complete body. */
std::uint64_t BuildWwiseStateCommand(std::uint32_t group,
                                     std::uint32_t state,
                                     std::uint16_t flags_a,
                                     std::uint16_t flags_b) {
    constexpr std::uint32_t kInvalidState = 0x2ca33bdbU;
    const auto normalized_state = state == kInvalidState ? 0U : state;

    alignas(8) std::byte scope[8]{};
    BeginAudioCommandScope(scope, 7, 0x10);
    auto* command = GetAudioCommandScope(scope);
    const auto normalized_flags_a = static_cast<std::uint16_t>(flags_a & 1U);
    const auto normalized_flags_b = static_cast<std::uint16_t>(flags_b & 1U);
    std::memcpy(command + 0x04, &group, sizeof(group));
    std::memcpy(command + 0x08, &normalized_state, sizeof(normalized_state));
    std::memcpy(command + 0x0c, &normalized_flags_a,
                sizeof(normalized_flags_a));
    std::memcpy(command + 0x0e, &normalized_flags_b,
                sizeof(normalized_flags_b));
    CommitAudioCommandScope(scope);
    return 1;
}

}  // namespace NRadEngine::nevr_reconstruction
