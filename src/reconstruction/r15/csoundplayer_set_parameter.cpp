// Quest libr15.so VA 0x144c12c; ReVault binary 576.
// ReVault raw parent 100227796, reconstruction node 100600507; grade M.
#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x144c12c — CSoundPlayer::SetParameter
/* @confidence: M — parameter scaling, sentinel extension and error path match the raw body; the unused object ABI remains opaque. */
std::uint64_t SetParameter(std::uint32_t symbol, float value,
                           std::uint32_t playing_id) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return 1;
    }

    std::uint64_t object_id = playing_id;
    if (playing_id == 0xffffffffU) {
        object_id = 0xffffffffffffffffULL;
    }

    const auto scaled_value = static_cast<std::int32_t>(value * 1000.0F);
    const auto result = QueueWwiseValueCommand(
        symbol, object_id, value, static_cast<std::uint32_t>(scaled_value), 4, 0);
    if (static_cast<std::int32_t>(result) == 1) {
        return result;
    }

    const auto error = TranslateWwiseResult(
        "Wwise error", static_cast<std::uint32_t>(result),
        "d:\\projects\\rad\\dev/src/engine/libs\\audio/wwise/wwiseresulttranslator.h",
        0x28);
    return static_cast<std::uint64_t>(OutputError(error, 8) == 0);
}

}  // namespace NRadEngine::AudioPost
