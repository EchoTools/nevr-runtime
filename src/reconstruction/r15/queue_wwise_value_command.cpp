// Quest libr15.so VA 0x2a49d50; ReVault binary 576.
// ReVault raw parent 100335353, reconstruction node 100600504; grade M.
#include "wwise_post.hpp"

namespace NRadEngine::nevr_reconstruction {

// @0x2a49d50 — QueueWwiseValueCommand
/* @confidence: M — the wrapper's argument forwarding and success return match the stored ARM64 body. */
std::uint64_t QueueWwiseValueCommand(std::uint64_t symbol,
                                     std::uint64_t game_object, float value,
                                     std::uint32_t scaled_value,
                                     std::uint32_t command_kind,
                                     std::uint32_t flags) {
    BuildWwiseValueCommand(static_cast<std::uint32_t>(symbol), game_object,
                           value, 0, scaled_value, command_kind, flags & 1U);
    return 1;
}

}  // namespace NRadEngine::nevr_reconstruction
