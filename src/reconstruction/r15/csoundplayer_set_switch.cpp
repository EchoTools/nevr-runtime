// Quest libr15.so VA 0x144bb4c; ReVault binary 576.
// ReVault raw parent 100227787, reconstruction node 100600528; grade M.
#include "wwise_post.hpp"

#include <algorithm>

namespace NRadEngine::nevr_reconstruction {
namespace {

void LockSwitchCache(std::atomic<std::int32_t>* lock) {
    std::int32_t expected = 0;
    while (!lock->compare_exchange_weak(expected, 1,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        expected = 0;
    }
}

void UnlockSwitchCache(std::atomic<std::int32_t>* lock) {
    lock->store(0, std::memory_order_release);
}

}  // namespace

// @0x144bb4c — CSoundPlayer::SetSwitch
/* @confidence: M — cache search/update, emitter selection, Wwise post, and both error reports follow the complete body; cache container operations are interpreted from the observed layout. */
std::uint64_t SetSwitch(const void* player, std::uint32_t group,
                        std::uint32_t value, std::uint32_t emitter_id) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return 1;
    }

    const auto* player_bytes = static_cast<const std::byte*>(player);
    const std::byte* emitter = nullptr;
    auto* lock_state =
        *reinterpret_cast<std::int32_t* const*>(player_bytes + 0x58);
    auto* lock = reinterpret_cast<std::atomic<std::int32_t>*>(lock_state);
    LockSwitchCache(lock);

    const auto* cached_entries =
        *reinterpret_cast<const std::byte* const*>(player_bytes + 0x60);
    const auto cached_count =
        *reinterpret_cast<const std::uint64_t*>(player_bytes + 0x90);
    std::uint64_t cached_index = 0;
    std::uint64_t cached_end = cached_count;
    bool cached_emitter_found = false;
    while (cached_index < cached_end) {
        const auto middle = cached_index + (cached_end - cached_index) / 2;
        const auto cached_group = *reinterpret_cast<const std::uint32_t*>(
            cached_entries + middle * 0x10);
        if (cached_group < emitter_id) {
            cached_index = middle + 1;
        } else if (cached_group > emitter_id) {
            cached_end = middle;
        } else {
            emitter = *reinterpret_cast<const std::byte* const*>(
                cached_entries + middle * 0x10 + 8);
            cached_emitter_found = true;
            break;
        }
    }
    UnlockSwitchCache(lock);
    if (cached_emitter_found && emitter == nullptr) {
        return 0;
    }
    if (!cached_emitter_found) {
        emitter = *reinterpret_cast<const std::byte* const*>(
            player_bytes + 0x238);
        if (emitter == nullptr ||
            *reinterpret_cast<const std::uint32_t*>(emitter + 0xb8) !=
                emitter_id) {
            const auto emitter_count =
                *reinterpret_cast<const std::uint64_t*>(player_bytes + 0x2c0);
            const std::byte* const* emitters =
                *reinterpret_cast<const std::byte* const* const*>(
                    player_bytes + 0x290);
            emitter = nullptr;
            for (std::uint64_t index = 0; index < emitter_count; ++index) {
                const auto* candidate = emitters[index];
                if (*reinterpret_cast<const std::uint32_t*>(candidate + 0xb8) ==
                    emitter_id) {
                    emitter = candidate;
                    break;
                }
            }
        }
    }
    if (emitter == nullptr) {
        return 0;
    }

    auto* table = const_cast<std::byte*>(emitter + 0x78);
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
        if (found->value == value) {
            return 1;
        }
        found->value = value;
    } else {
        const SoundSwitchKey new_key{group, value};
        InsertSoundSwitchKey(table, static_cast<std::uint64_t>(found - entries),
                             &new_key, 1);
    }

    const auto result = QueueWwiseSwitchCommand(group, value, emitter_id);
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
    OutputError(SoundSymbolError(group, "setting switch", "switch group"), 8);
    if (value != 0) {
        OutputError(SoundSymbolError(value, "setting switch", "switch"), 8);
    }
    return 0;
}

}  // namespace NRadEngine::nevr_reconstruction
