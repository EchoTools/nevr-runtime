#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x144c084 — CSoundPlayer::GetState
/* @confidence: M — disabled-audio behavior, output initialization, result translation, conditional symbol error, and return value match the complete body; state-query semantics remain at the named Wwise helper boundary. */
std::uint32_t GetState(std::uint32_t group) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return 0;
    }

    std::uint32_t state = 0;
    const auto result = QueryWwiseState(group, &state);
    if (result != 1) {
        const auto error = TranslateWwiseResult(
            "Wwise error", static_cast<std::uint32_t>(result),
            "d:\\projects\\rad\\dev/src/engine/libs\\audio/wwise/wwiseresulttranslator.h",
            0x28);
        if (OutputError(error, 8) != 0) {
            const auto symbol_error =
                SoundSymbolError(group, "fetching state", "switch group");
            OutputError(symbol_error, 8);
            return 0;
        }
    }
    return state;
}

}  // namespace NRadEngine::AudioPost
