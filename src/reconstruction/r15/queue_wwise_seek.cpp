// Quest libr15.so VA 0x2a4c1a0; ReVault binary 576.
// ReVault raw parent 100335440, reconstruction node 100600352; grade M.
#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2a4c1a0 — QueueWwiseSeek
/* @confidence: M — command field widths and raw queue sequence are preserved. */
std::uint32_t QueueWwiseSeek(std::uint32_t event, std::uint64_t game_object,
                             std::uint32_t seek_ms, std::uint16_t flags,
                             std::uint32_t playing_id) {
    auto* symbol = FindWwiseEventSymbol(DAT_038cec70 + 0xc0, event);
    if (symbol == nullptr) {
        return 2;
    }

    std::array<std::byte, 8> command_scope{};
    const auto command_size = WwiseSeekCommandSize();
    BeginAudioCommandScope(command_scope.data(), 0x27, command_size);
    auto* command = GetAudioCommandScope(command_scope.data());
    StoreAudioCommandReference(command + 0x0c, symbol);
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint32_t*>(command + 0x14) = event;
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint64_t*>(command + 0x04) = game_object;
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint32_t*>(command + 0x1c) = playing_id;
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint16_t*>(command + 0x20) = 0;
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint32_t*>(command + 0x18) = seek_ms;
    command = GetAudioCommandScope(command_scope.data());
    *reinterpret_cast<std::uint16_t*>(command + 0x22) =
        static_cast<std::uint16_t>(flags & 1U);
    CommitAudioCommandScope(command_scope.data());
    return 1;
}

}  // namespace NRadEngine::AudioPost
