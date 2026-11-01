#include "wwise_post.hpp"
#include "cmemory_context.hpp"

#include <algorithm>
#include <cstring>

namespace NRadEngine::CSoundPlayer {
void* CreateEmitterNoLock(void* player, const char* emitter_name,
                          std::uint32_t one_off, std::uint32_t flags);
void DestroyEmitterNoLock(std::uint64_t player, std::uint64_t emitter);
}

namespace NRadEngine::CSoundEmitter {
void SetPosition(void* emitter, const void* transform);
}

namespace NRadEngine::CMemBlock {
void Resize(void* block, std::uint64_t bytes);
}

namespace NRadEngine::AudioPost {
namespace {

void LockPlayerAudioMutex(void* mutex) {
    auto* lock = reinterpret_cast<std::atomic<std::int32_t>*>(mutex);
    std::int32_t expected = 0;
    while (!lock->compare_exchange_weak(expected, 1,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        expected = 0;
    }
}

void UnlockPlayerAudioMutex(void* mutex) {
    reinterpret_cast<std::atomic<std::int32_t>*>(mutex)->store(
        0, std::memory_order_release);
}

template <typename T>
T& PlayerField(void* player, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(player) + offset);
}

std::uint64_t FindSoundKeyIndex(const SoundKey* entries, std::uint64_t count,
                                std::uint32_t key) {
    std::uint64_t first = 0;
    std::uint64_t last = count;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        if (entries[middle].playing_id < key) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    return first;
}

}  // namespace

// @0x144b1b0 — CSoundPlayer::PlayOneOff
/* @confidence: M — all 174 instructions are represented, including lock order, emitter and sound pool updates, optional Wwise posting, table insertion, and queued-sound growth; helper parameter names and object-field meanings follow the ABI and observed offsets. */
void PlayOneOff(void* player, const char* emitter_name,
                std::uint32_t sound_symbol, const void* transform,
                std::uint32_t post_symbol, float volume) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return;
    }

    auto* const sound_lock =
        *reinterpret_cast<void**>(static_cast<std::byte*>(player) + 0xe0);
    LockPlayerAudioMutex(sound_lock);
    if (PlayerField<std::uint64_t>(player, 0x118) == 0) {
        UnlockPlayerAudioMutex(sound_lock);
        return;
    }

    auto* const emitter_lock =
        *reinterpret_cast<void**>(static_cast<std::byte*>(player) + 0x58);
    LockPlayerAudioMutex(emitter_lock);
    if (PlayerField<std::uint64_t>(player, 0xd0) == 0) {
        NRadEngine::NWriteLog::WriteLog(
            4, nullptr,
            "Attempting to create a one off emitter when max emitters has been reached. Sound failed to play. Contact the sound team.");
        UnlockPlayerAudioMutex(emitter_lock);
        UnlockPlayerAudioMutex(sound_lock);
        return;
    }

    auto* const emitter = NRadEngine::CSoundPlayer::CreateEmitterNoLock(
        player, emitter_name, 1, 0);
    NRadEngine::CSoundEmitter::SetPosition(emitter, transform);
    PlayerField<std::uint32_t>(emitter, 0xbc) |= 0x10U;

    if (post_symbol != 0) {
        const auto result = QueueWwiseValueCommand(
            post_symbol, PlayerField<std::uint32_t>(emitter, 0xb8), volume, 0, 4,
            0);
        if (result != 1) {
            const auto error = TranslateWwiseResult(
                "Wwise error", static_cast<std::uint32_t>(result),
                "d:\\projects\\rad\\dev/src/engine/libs\\audio/wwise/wwiseresulttranslator.h",
                0x28);
            OutputError(error, 8);
        }
    }

    auto& sound_count = PlayerField<std::uint64_t>(player, 0x118);
    if (sound_count == 0) {
        NRadEngine::NWriteLog::WriteLog(
            4, nullptr,
            "Attempting to create a sound when max sounds has been reached. Sound failed to play. Contact the sound team.");
        NRadEngine::CSoundPlayer::DestroyEmitterNoLock(
            reinterpret_cast<std::uint64_t>(player),
            reinterpret_cast<std::uint64_t>(emitter));
        UnlockPlayerAudioMutex(emitter_lock);
        UnlockPlayerAudioMutex(sound_lock);
        return;
    }

    auto** const sound_pool =
        *reinterpret_cast<void***>(static_cast<std::byte*>(player) + 0xe8);
    auto* const sound = sound_pool[sound_count - 1];
    --sound_count;
    if (sound == nullptr) {
        NRadEngine::CSoundPlayer::DestroyEmitterNoLock(
            reinterpret_cast<std::uint64_t>(player),
            reinterpret_cast<std::uint64_t>(emitter));
        UnlockPlayerAudioMutex(emitter_lock);
        UnlockPlayerAudioMutex(sound_lock);
        return;
    }

    PlayerField<std::uint32_t>(sound, 0x4c) = 1;
    PlayerField<std::uint32_t>(sound, 0x38) = sound_symbol;
    PlayerField<std::uint32_t>(sound, 0x3c) = UINT32_MAX;
    PlayerField<std::uint32_t>(sound, 0x44) = 1;
    const auto emitter_id = PlayerField<std::uint32_t>(emitter, 0xb8);
    PlayerField<std::uint32_t>(sound, 0x40) = emitter_id;

    auto* const emitter_table = static_cast<std::byte*>(emitter) + 0x38;
    const auto emitter_count =
        *reinterpret_cast<const std::uint64_t*>(emitter_table + 0x30);
    auto* const emitter_entries =
        *reinterpret_cast<const SoundKey**>(emitter_table);
    const auto missing_key = FindSoundKeyIndex(
        emitter_entries, emitter_count, UINT32_MAX);
    if (missing_key == emitter_count ||
        emitter_entries[missing_key].playing_id != UINT32_MAX) {
        const SoundKey entry{UINT32_MAX, 0,
                             reinterpret_cast<std::uintptr_t>(sound)};
        InsertSoundKey(emitter_table, missing_key, 1, &entry);
    }

    UnlockPlayerAudioMutex(emitter_lock);
    UnlockPlayerAudioMutex(sound_lock);

    auto* const queue_lock =
        *reinterpret_cast<void**>(static_cast<std::byte*>(player) + 0x230);
    LockPlayerAudioMutex(queue_lock);
    auto& queue_count = PlayerField<std::uint64_t>(player, 0x220);
    auto& queue_capacity = PlayerField<std::uint64_t>(player, 0x218);
    if (queue_count >= queue_capacity) {
        const auto growth = std::max(
            queue_capacity, PlayerField<std::uint64_t>(player, 0x210));
        NRadEngine::CMemBlock::Resize(
            static_cast<std::byte*>(player) + 0x1f0,
            (queue_capacity + growth) * sizeof(void*));
        queue_capacity += growth;
    }

    auto** queue =
        *reinterpret_cast<void***>(static_cast<std::byte*>(player) + 0x1f0);
    auto* const previous_allocator = NRadEngine::CMemoryContext::CurrentPtr();
    NRadEngine::CMemoryContext::SetCurrent(
        reinterpret_cast<NRadEngine::CAllocator*>(
            PlayerField<std::uintptr_t>(player, 0x200)));
    queue[queue_count++] = sound;
    NRadEngine::CMemoryContext::SetCurrent(previous_allocator);
    UnlockPlayerAudioMutex(queue_lock);
}

}  // namespace NRadEngine::AudioPost
