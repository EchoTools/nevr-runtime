#include "wwise_post.hpp"

#include <algorithm>

namespace NRadEngine::AudioPost {

// @0x144bf10 — CSoundPlayer::SetState
/* @confidence: M — sorted cache handling, duplicate suppression, command submission and translated errors match the raw body; sorting semantics are interpreted from the table layout. */
std::uint64_t SetState(void* player, std::uint32_t group,
                       std::uint32_t state) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return 1;
    }

    auto* table = static_cast<std::byte*>(player) + 0x338;
    auto* entries = *reinterpret_cast<SoundSwitchKey**>(table);
    auto count = *reinterpret_cast<std::uint64_t*>(table + 0x30);
    auto* sort_flags = reinterpret_cast<std::uint32_t*>(table + 0x38);
    if ((*sort_flags & 1U) != 0) {
        std::sort(entries, entries + count,
                  [](const SoundSwitchKey& left, const SoundSwitchKey& right) {
                      return left.group < right.group;
                  });
        *sort_flags &= ~1U;
        entries = *reinterpret_cast<SoundSwitchKey**>(table);
        count = *reinterpret_cast<std::uint64_t*>(table + 0x30);
    }

    auto* found = std::lower_bound(
        entries, entries + count, group,
        [](const SoundSwitchKey& key, std::uint32_t wanted_group) {
            return key.group < wanted_group;
        });
    if (found != entries + count && found->group == group) {
        if (found->value == state) {
            return 1;
        }
        found->value = state;
    } else {
        const SoundSwitchKey new_key{group, state};
        InsertSoundSwitchKey(table, static_cast<std::uint64_t>(found - entries),
                             &new_key, 1);
    }

    const auto result = QueueWwiseStateCommand(group, state);
    if (result == 1) {
        return result;
    }

    const auto error = TranslateWwiseResult(
        "Wwise error", static_cast<std::uint32_t>(result),
        "d:\\projects\\rad\\dev/src/engine/libs\\audio/wwise/wwiseresulttranslator.h",
        0x28);
    if (OutputError(error, 8) == 0) {
        return 1;
    }
    OutputError(SoundSymbolError(group, "setting state", "state group"), 8);
    OutputError(SoundSymbolError(state, "setting state", "state"), 8);
    return 0;
}

}  // namespace NRadEngine::AudioPost
