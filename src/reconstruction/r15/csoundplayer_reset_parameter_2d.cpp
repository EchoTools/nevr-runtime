// Quest libr15.so VA 0x144c39c; ReVault binary 576.
// ReVault raw parent 100227804, reconstruction node 100600510; grade M.
#include "wwise_post.hpp"

namespace NRadEngine::nevr_reconstruction {

// @0x144c39c — CSoundPlayer::ResetParameter2D
/* @confidence: M — emitter and symbol guards, ID extension, scaling and error path match the complete stored body. */
std::uint64_t ResetParameter2D(const void* player, std::uint32_t symbol,
                               float value) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return 1;
    }

    const auto* player_bytes = static_cast<const std::byte*>(player);
    const auto* emitter = *reinterpret_cast<const std::byte* const*>(
        player_bytes + 0x238);
    if (emitter == nullptr || symbol == 0) {
        return 0;
    }

    const auto object_id =
        *reinterpret_cast<const std::uint32_t*>(emitter + 0xb8);
    std::uint64_t extended_object_id = object_id;
    if (object_id == 0xffffffffU) {
        extended_object_id = 0xffffffffffffffffULL;
    }

    const auto scaled_value = static_cast<std::int32_t>(value * 1000.0F);
    const auto result = QueueWwiseResetCommand(
        symbol, extended_object_id, static_cast<std::uint32_t>(scaled_value),
        4, 0);
    if (static_cast<std::int32_t>(result) == 1) {
        return result;
    }

    const auto error = TranslateWwiseResult(
        "Wwise error", static_cast<std::uint32_t>(result),
        "d:\\projects\\rad\\dev/src/engine/libs\\audio/wwise/wwiseresulttranslator.h",
        0x28);
    return static_cast<std::uint64_t>(OutputError(error, 8) == 0);
}

}  // namespace NRadEngine::nevr_reconstruction
