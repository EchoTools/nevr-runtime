// Quest libr15.so VA 0x144bd8c; ReVault binary 576.
// ReVault raw parent 100227793, reconstruction node 100600533; grade M.
#include "wwise_post.hpp"

#include <algorithm>

namespace NRadEngine::AudioPost {
namespace {

void LockSwitchLookup(std::atomic<std::int32_t>* lock) {
    std::int32_t expected = 0;
    while (!lock->compare_exchange_weak(expected, 1,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        expected = 0;
    }
}

void UnlockSwitchLookup(std::atomic<std::int32_t>* lock) {
    lock->store(0, std::memory_order_release);
}

const std::byte* FindSoundEmitter(const std::byte* player,
                                  std::uint32_t emitter_id) {
    auto* lock_state =
        *reinterpret_cast<std::int32_t* const*>(player + 0x58);
    auto* lock = reinterpret_cast<std::atomic<std::int32_t>*>(lock_state);
    LockSwitchLookup(lock);

    const auto* cached_entries =
        *reinterpret_cast<const std::byte* const*>(player + 0x60);
    const auto cached_count =
        *reinterpret_cast<const std::uint64_t*>(player + 0x90);
    std::uint64_t first = 0;
    std::uint64_t last = cached_count;
    const std::byte* emitter = nullptr;
    bool found_in_cache = false;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        const auto key = *reinterpret_cast<const std::uint32_t*>(
            cached_entries + middle * 0x10);
        if (key < emitter_id) {
            first = middle + 1;
        } else if (key > emitter_id) {
            last = middle;
        } else {
            emitter = *reinterpret_cast<const std::byte* const*>(
                cached_entries + middle * 0x10 + 8);
            found_in_cache = true;
            break;
        }
    }
    UnlockSwitchLookup(lock);
    if (found_in_cache) {
        return emitter;
    }

    emitter = *reinterpret_cast<const std::byte* const*>(player + 0x238);
    if (emitter != nullptr &&
        *reinterpret_cast<const std::uint32_t*>(emitter + 0xb8) == emitter_id) {
        return emitter;
    }
    const auto emitter_count =
        *reinterpret_cast<const std::uint64_t*>(player + 0x2c0);
    const std::byte* const* emitters =
        *reinterpret_cast<const std::byte* const* const*>(player + 0x290);
    for (std::uint64_t index = 0; index < emitter_count; ++index) {
        const auto* candidate = emitters[index];
        if (*reinterpret_cast<const std::uint32_t*>(candidate + 0xb8) ==
            emitter_id) {
            return candidate;
        }
    }
    return nullptr;
}

}  // namespace

// @0x144bd8c — CSoundPlayer::GetSwitch
/* @confidence: M — emitter resolution, optional cache sort and key lookup match the raw body; the sort operation is interpreted from the table fields. */
std::uint32_t GetSwitch(const void* player, std::uint32_t group,
                        std::uint32_t emitter_id) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return 0;
    }

    const auto* player_bytes = static_cast<const std::byte*>(player);
    const auto* emitter = FindSoundEmitter(player_bytes, emitter_id);
    if (emitter == nullptr) {
        return 0;
    }

    const auto count =
        *reinterpret_cast<const std::uint64_t*>(emitter + 0xa8);
    if (count == 0) {
        return 0;
    }

    auto* table = const_cast<std::byte*>(emitter + 0x78);
    auto* entries = *reinterpret_cast<SoundSwitchKey**>(table);
    auto* sort_flags = reinterpret_cast<std::uint32_t*>(table + 0x38);
    if ((*sort_flags & 1U) != 0) {
        std::sort(entries, entries + count,
                  [](const SoundSwitchKey& left, const SoundSwitchKey& right) {
                      return left.group < right.group;
                  });
        *sort_flags &= ~1U;
    }

    const auto* found = std::lower_bound(
        entries, entries + count, group,
        [](const SoundSwitchKey& key, std::uint32_t wanted_group) {
            return key.group < wanted_group;
        });
    return found != entries + count && found->group == group ? found->value : 0;
}

}  // namespace NRadEngine::AudioPost
