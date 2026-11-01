// Quest libr15.so VA 0x2aad070; ReVault binary 576.
// ReVault raw parent 100340236, reconstruction node 100600429; grade M.
#include "wwise_post.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2aad070 — SubmitWwisePostCommand
/* @confidence: M — mirrors the raw allocation, node initialization, metadata copy, flag mask and enqueue sequence. */
int SubmitWwisePostCommand(std::uint64_t context, std::byte* command,
                           SoundEventCallback callback, void* cookie,
                           std::uint32_t flags, std::uint32_t thread_token) {
    std::array<std::byte, 8> symbol_lookup_scope{};
    BeginWwiseSymbolLookup(
        symbol_lookup_scope.data(),
        static_cast<std::uintptr_t>(context + 0x18));

    auto* allocation = AllocateWwisePoolBlock(DAT_03859620, 0x70);
    int result = 2;
    if (allocation != nullptr) {
        std::array<std::byte, 8> placement_context{};
        BeginWwisePlacementContext(placement_context.data());
        auto* node = static_cast<std::byte*>(
            WwisePlacementStorage(0x70, allocation));
        if (node != nullptr) {
            std::fill_n(node, 0x70, std::byte{});
            InitializeAudioPostCommandNode(node);
        }

        *reinterpret_cast<std::uint64_t*>(allocation + 0x20) =
            0x100000000ULL;
        *reinterpret_cast<std::uint32_t*>(allocation + 0x28) = thread_token;
        *reinterpret_cast<std::uint64_t*>(allocation + 0x30) =
            *reinterpret_cast<const std::uint64_t*>(command);
        InitializeWwiseCallbackMetadata(
            allocation + 0x38,
            *reinterpret_cast<const std::uint32_t*>(command + 8),
            command + 16);

        auto stored_flags = flags;
        if (callback == nullptr) {
            stored_flags &= 0xfff02004U;
        }
        *reinterpret_cast<SoundEventCallback*>(allocation + 0x50) = callback;
        *reinterpret_cast<void**>(allocation + 0x58) = cookie;
        *reinterpret_cast<std::uint32_t*>(allocation + 0x60) = stored_flags;
        InsertWwisePostNode(context, allocation);
        result = 1;
    }

    EndWwiseSymbolLookup(symbol_lookup_scope.data());
    return result;
}

}  // namespace NRadEngine::AudioPost
