// Quest libr15.so VA 0x1447ae0; ReVault binary 576.
// ReVault raw parent 100227711, reconstruction node 100600443; grade M.
// @0x1447ae0 — NRadEngine::CSoundPlayer::DestroySoundNoLock
/* @confidence: M */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {
namespace {
template <typename T>
T& at(void* object, std::size_t offset) {
  return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}
template <typename T>
const T& at(const void* object, std::size_t offset) {
  return *reinterpret_cast<const T*>(static_cast<const std::byte*>(object) + offset);
}
struct SoundKey {
  std::uint32_t key;
  std::uint32_t padding;
  void* value;
};
static_assert(sizeof(SoundKey) == 0x10);
static_assert(offsetof(SoundKey, key) == 0x00);
static_assert(offsetof(SoundKey, padding) == 0x04);
static_assert(offsetof(SoundKey, value) == 0x08);
}

namespace NWriteLog {
void WriteLog(int severity, int category, const char* message);
}
namespace CMemBlock {
void Resize(void* block, std::size_t bytes);
}
namespace CMemoryContext {
void* CurrentPtr();
void SetCurrent(void* context);
}
namespace CSoundEmitter {
void DestroySound(void* emitter, std::uint32_t playing_id);
}

void CSoundPlayer_DestroySoundNoLock(void* player, void* sound) {
  auto& pool_capacity = at<std::uint64_t>(player, 0x110);
  auto& pool_count = at<std::uint64_t>(player, 0x118);
  if (pool_count == pool_capacity) {
    NWriteLog::WriteLog(
        4, 0,
        "Attempting to destroy a sound when the sound pool is full. Contact the sound team.");
    return;
  }

  const auto playing_id = at<std::uint32_t>(sound, 0x3c);
  if (playing_id != UINT32_MAX) {
    auto& player_table_count = at<std::uint64_t>(player, 0x150);
    if (player_table_count != 0) {
      auto* entries = reinterpret_cast<SoundKey*>(at<std::uintptr_t>(player, 0x120));
      auto& table_flags = at<std::uint32_t>(player, 0x158);
      if ((table_flags & 1u) != 0) {
        std::sort(entries, entries + player_table_count,
                  [](const SoundKey& left, const SoundKey& right) {
                    return left.key < right.key;
                  });
        table_flags &= ~1u;
      }

      std::size_t first = 0;
      std::size_t last = player_table_count;
      while (first < last) {
        const std::size_t middle = first + (last - first) / 2;
        if (entries[middle].key < playing_id) {
          first = middle + 1;
        } else {
          last = middle;
        }
      }
      if (first < player_table_count && entries[first].key == playing_id) {
        const std::size_t following = first + 1;
        const std::size_t remaining = player_table_count - following;
        if (remaining != 0) {
          std::memmove(entries + first, entries + following,
                       remaining * sizeof(SoundKey));
        }
        --player_table_count;
      }
    }
  }

  const auto emitter_id = at<std::uint32_t>(sound, 0x40);
  if (emitter_id != UINT32_MAX) {
    auto* emitter_entries =
        reinterpret_cast<SoundKey*>(at<std::uintptr_t>(player, 0x60));
    const auto emitter_count = at<std::uint64_t>(player, 0x90);
    std::size_t first = 0;
    std::size_t last = emitter_count;
    void* emitter = nullptr;

    while (first < last) {
      const std::size_t middle = first + (last - first) / 2;
      if (emitter_entries[middle].key < emitter_id) {
        first = middle + 1;
      } else if (emitter_entries[middle].key == emitter_id) {
        emitter = emitter_entries[middle].value;
        break;
      } else {
        last = middle;
      }
    }

    if (emitter == nullptr) {
      void* current_emitter = reinterpret_cast<void*>(at<std::uintptr_t>(player, 0x238));
      if (current_emitter != nullptr &&
          at<std::uint32_t>(current_emitter, 0xb8) == emitter_id) {
        emitter = current_emitter;
      } else {
        auto** emitters = reinterpret_cast<void**>(at<std::uintptr_t>(player, 0x290));
        auto remaining = at<std::uint64_t>(player, 0x2c0);
        for (std::uint64_t index = 0; index < remaining; ++index) {
          void* candidate = emitters[index];
          if (at<std::uint32_t>(candidate, 0xb8) == emitter_id) {
            emitter = candidate;
            break;
          }
        }
      }
    }

    if (emitter != nullptr) {
      CSoundEmitter::DestroySound(emitter, playing_id);
    }
  }

  // Reset the SSound fields exactly as the stores at +0x38, +0x48 and +0x88 do.
  at<std::uint32_t>(sound, 0x38) = 0;
  at<std::uint32_t>(sound, 0x3c) = UINT32_MAX;
  at<std::uint32_t>(sound, 0x40) = UINT32_MAX;
  at<std::uint32_t>(sound, 0x44) = 0;
  at<std::uint64_t>(sound, 0x48) = 0x800000;
  at<std::uint64_t>(sound, 0x88) = 0;

  auto& allocated_pool_capacity = at<std::uint64_t>(player, 0x108);
  if (pool_count >= pool_capacity) {
    const std::uint64_t growth =
        pool_capacity <= allocated_pool_capacity ? allocated_pool_capacity
                                                  : pool_capacity;
    CMemBlock::Resize(static_cast<std::byte*>(player) + 0xe8,
                      static_cast<std::size_t>(growth + pool_capacity) * 8);
    pool_capacity += growth;
  }

  auto** pool = reinterpret_cast<void**>(at<std::uintptr_t>(player, 0xe8));
  void* previous_context = CMemoryContext::CurrentPtr();
  CMemoryContext::SetCurrent(
      reinterpret_cast<void*>(at<std::uintptr_t>(player, 0xf8)));
  pool[pool_count++] = sound;
  CMemoryContext::SetCurrent(previous_context);
}
}  // namespace NRadEngine
