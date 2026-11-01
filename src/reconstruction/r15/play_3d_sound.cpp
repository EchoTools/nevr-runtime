#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {
void CSoundPlayer_DestroySoundNoLock(void* player, void* sound);
namespace CSoundPlayer {
void DestroyEmitterNoLock(std::uint64_t player, std::uint64_t emitter);
}

namespace NWriteLog {
void WriteLog(int severity, int category, const char* format, ...);
}

}  // namespace NRadEngine

namespace NRadEngine::AudioPost {
namespace {

std::byte* at(std::uint64_t object, std::size_t offset) {
    return reinterpret_cast<std::byte*>(object + offset);
}

template <typename T>
T load(std::uint64_t object, std::size_t offset) {
    T value{};
    std::memcpy(&value, at(object, offset), sizeof(value));
    return value;
}

template <typename T>
void store(std::uint64_t object, std::size_t offset, T value) {
    std::memcpy(at(object, offset), &value, sizeof(value));
}

void acquire(std::uint64_t lock_address) {
    auto* lock = reinterpret_cast<std::uint32_t*>(lock_address);
    for (;;) {
        while (__atomic_load_n(lock, __ATOMIC_ACQUIRE) != 0) {
        }
        std::uint32_t unlocked = 0;
        if (__atomic_compare_exchange_n(lock, &unlocked, 1, true,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
    }
}

void release(std::uint64_t lock_address) {
    __atomic_store_n(reinterpret_cast<std::uint32_t*>(lock_address), 0,
                     __ATOMIC_RELEASE);
}

std::uint64_t find_emitter(std::uint64_t player, std::uint32_t emitter_id) {
    const auto count = load<std::uint64_t>(player, 0x90);
    const auto entries = load<std::uint64_t>(player, 0x60);
    std::uint64_t first = 0;
    auto last = count;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        const auto key = load<std::uint32_t>(entries + middle * 0x10, 0);
        if (key < emitter_id) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    if (first < count && load<std::uint32_t>(entries + first * 0x10, 0) == emitter_id) {
        return load<std::uint64_t>(entries + first * 0x10, 8);
    }

    auto emitter = load<std::uint64_t>(player, 0x238);
    if (emitter != 0 && load<std::uint32_t>(emitter, 0xb8) == emitter_id) {
        return emitter;
    }

    auto remaining = load<std::uint64_t>(player, 0x2c0);
    auto* emitters = reinterpret_cast<const std::uint64_t*>(
        load<std::uint64_t>(player, 0x290));
    while (remaining != 0) {
        emitter = *emitters++;
        if (load<std::uint32_t>(emitter, 0xb8) == emitter_id) {
            return emitter;
        }
        --remaining;
    }
    return 0;
}

std::uint64_t find_key_index(std::uint64_t entries, std::uint64_t count,
                             std::uint32_t key) {
    std::uint64_t first = 0;
    auto last = count;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        const auto value = load<std::uint32_t>(entries + middle * 0x10, 0);
        if (value < key) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    return first;
}

}  // namespace

// @0x144b468 — NRadEngine::CSoundPlayer::Play3DSound
/* @confidence: M — raw control flow and object offsets are transcribed; the engine-owned global bindings and callback ABI remain external. */
std::int32_t Play3DSound(std::uint64_t player, std::uint64_t event,
                         std::uint32_t emitter_id, float volume) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return 0;
    }

    acquire(load<std::uint64_t>(player, 0x58));
    auto emitter = find_emitter(player, emitter_id);
    release(load<std::uint64_t>(player, 0x58));
    if (emitter == 0) {
        return -1;
    }

    const auto sound_lock = load<std::uint64_t>(player, 0xe0);
    acquire(sound_lock);
    auto pool_size = load<std::uint64_t>(player, 0x118);
    if (pool_size == 0) {
        NWriteLog::WriteLog(
            4, 0,
            "Attempting to create a sound when max sounds has been reached. Sound failed to play. Contact the sound team.");
        release(sound_lock);
        return -1;
    }

    const auto pool = load<std::uint64_t>(player, 0xe8);
    const auto sound = load<std::uint64_t>(pool + (pool_size - 1) * sizeof(std::uint64_t), 0);
    store(player, 0x118, pool_size - 1);
    release(sound_lock);

    std::int32_t playing_id = -1;
    if (sound == 0) {
        if ((load<std::uint8_t>(emitter, 0xbc) & 1U) == 0) {
            return -1;
        }
    } else {
        const auto flags = load<std::uint32_t>(player, 0x244);
        const auto result = PostSoundEvent(
            reinterpret_cast<void*>(g_wwise_engine), static_cast<std::uint32_t>(event),
            emitter_id, &flags, g_sound_event_callback,
            reinterpret_cast<void*>(sound),
            static_cast<std::uint32_t>(static_cast<std::int32_t>(volume * 1000.0F)),
            &playing_id);
        if (OutputError(static_cast<std::uint64_t>(result), 8) != 0) {
            acquire(sound_lock);
            CSoundPlayer_DestroySoundNoLock(reinterpret_cast<void*>(player),
                                            reinterpret_cast<void*>(sound));
            release(sound_lock);
            if ((load<std::uint8_t>(emitter, 0xbc) & 1U) == 0) {
                return -1;
            }
        } else {
            acquire(sound_lock);
            store(sound, 0x44, std::uint32_t{1});
            store(sound, 0x38, static_cast<std::uint32_t>(event));
            store(sound, 0x3c, static_cast<std::uint32_t>(playing_id));
            store(sound, 0x40, load<std::uint32_t>(emitter, 0xb8));

            const auto emitter_count = load<std::uint64_t>(emitter, 0x68);
            auto index = find_key_index(load<std::uint64_t>(emitter, 0x38),
                                        emitter_count,
                                        static_cast<std::uint32_t>(playing_id));
            const SoundKey emitter_key{static_cast<std::uint32_t>(playing_id), 0, sound};
            InsertSoundKey(reinterpret_cast<void*>(emitter + 0x38), index, 1,
                           &emitter_key);

            const auto player_table = player + 0x120;
            const auto player_count = load<std::uint64_t>(player, 0x150);
            index = find_key_index(load<std::uint64_t>(player_table, 0), player_count,
                                   static_cast<std::uint32_t>(playing_id));
            const SoundKey player_key{static_cast<std::uint32_t>(playing_id), 0, sound};
            InsertSoundKey(reinterpret_cast<void*>(player_table), index, 1,
                           &player_key);
            release(sound_lock);
            return playing_id;
        }
    }

    acquire(sound_lock);
    const auto emitter_lock = load<std::uint64_t>(player, 0x58);
    acquire(emitter_lock);
    CSoundPlayer::DestroyEmitterNoLock(player, emitter);
    release(emitter_lock);
    release(sound_lock);
    return -1;
}

}  // namespace NRadEngine::AudioPost
