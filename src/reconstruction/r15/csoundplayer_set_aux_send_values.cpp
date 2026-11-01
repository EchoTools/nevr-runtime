// Quest libr15.so VA 0x144b8b4; ReVault binary 576.
// ReVault raw parent 100227783, reconstruction node 100600543; grade M.
#include "wwise_post.hpp"

#include <cstring>

namespace NRadEngine::AudioPost {

// @0x144b8b4 — CSoundPlayer::SetAuxSendValues
/* @confidence: M — disabled-audio return, sentinel-prefixed 16-byte entries, command result translation, and error-dependent return match the full body; the table is treated as count plus an array of 64-bit values. */
std::uint64_t SetAuxSendValues(const void* values_table,
                               std::uint32_t object_id) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return 1;
    }

    const auto* table = static_cast<const std::byte*>(values_table);
    const auto count = *reinterpret_cast<const std::uint64_t*>(table + 0x30);
    std::byte entries[0x50];
    if (count != 0) {
        const auto* values =
            *reinterpret_cast<const std::uint64_t* const*>(table);
        for (std::uint64_t index = 0; index < count; ++index) {
            const std::uint64_t invalid_id = UINT64_MAX;
            std::memcpy(entries + index * 0x10, &invalid_id,
                        sizeof(invalid_id));
            std::memcpy(entries + index * 0x10 + 8, values + index,
                        sizeof(*values));
        }
    }

    const auto result =
        QueueWwiseAuxSendValues(object_id, entries,
                                static_cast<std::uint32_t>(count));
    if (result != 1) {
        const auto error = TranslateWwiseResult(
            "Wwise error", static_cast<std::uint32_t>(result),
            "d:\\projects\\rad\\dev/src/engine/libs\\audio/wwise/wwiseresulttranslator.h",
            0x28);
        return OutputError(error, 8) == 0 ? 1 : 0;
    }
    return result;
}

}  // namespace NRadEngine::AudioPost
