// Quest libr15.so VA 0x144b100; ReVault binary 576.
// ReVault raw parent 100227774, reconstruction node 100600538; grade M.
#include "wwise_post.hpp"

namespace NRadEngine::nevr_reconstruction {
namespace {

void LockSoundStateTable(std::atomic<std::int32_t>* lock) {
    std::int32_t expected = 0;
    while (!lock->compare_exchange_weak(expected, 1,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        expected = 0;
    }
}

void UnlockSoundStateTable(std::atomic<std::int32_t>* lock) {
    lock->store(0, std::memory_order_release);
}

}  // namespace

// @0x144b100 — CSoundPlayer::SoundStopped
/* @confidence: M — audio-disabled behavior, lock-protected binary search, all unlock paths, null handling, and inverse state comparison match the complete body; table field meaning is inferred from the access layout. */
bool SoundStopped(const void* player, std::uint32_t expected_state,
                  std::uint32_t entry_key) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return true;
    }

    const auto* player_bytes = static_cast<const std::byte*>(player);
    auto* lock_state =
        *reinterpret_cast<std::int32_t* const*>(player_bytes + 0xe0);
    auto* lock = reinterpret_cast<std::atomic<std::int32_t>*>(lock_state);
    LockSoundStateTable(lock);

    const auto count =
        *reinterpret_cast<const std::uint64_t*>(player_bytes + 0x150);
    const auto* entries =
        *reinterpret_cast<const SoundKey* const*>(player_bytes + 0x120);
    std::uint64_t first = 0;
    std::uint64_t last = count;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        const auto key = entries[middle].playing_id;
        if (key < entry_key) {
            first = middle + 1;
        } else if (key > entry_key) {
            last = middle;
        } else {
            const auto* sound =
                reinterpret_cast<const std::byte*>(entries[middle].sound);
            UnlockSoundStateTable(lock);
            return sound == nullptr ||
                   *reinterpret_cast<const std::uint32_t*>(sound + 0x40) !=
                       expected_state;
        }
    }

    UnlockSoundStateTable(lock);
    return true;
}

}  // namespace NRadEngine::nevr_reconstruction
