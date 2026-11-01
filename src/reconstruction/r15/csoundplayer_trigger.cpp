// Quest libr15.so VA 0x144b828; ReVault binary 576.
// ReVault raw parent 100227782, reconstruction node 100600541; grade H.
#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x144b828 — CSoundPlayer::Trigger
/* @confidence: H — audio-disabled return, Wwise command arguments, result translation, conditional symbol error, and final result follow the complete 35-instruction body. */
std::uint64_t Trigger(std::uint32_t trigger, std::uint32_t object_id) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return 1;
    }

    const auto result = QueueWwiseTrigger(trigger, object_id);
    if (result != 1) {
        const auto error = TranslateWwiseResult(
            "Wwise error", static_cast<std::uint32_t>(result),
            "d:\\projects\\rad\\dev/src/engine/libs\\audio/wwise/wwiseresulttranslator.h",
            0x28);
        if (OutputError(error, 8) == 0) {
            return 1;
        }
        const auto symbol_error =
            SoundSymbolError(trigger, "playing trigger", "trigger");
        OutputError(symbol_error, 8);
        return 0;
    }
    return result;
}

}  // namespace NRadEngine::AudioPost
