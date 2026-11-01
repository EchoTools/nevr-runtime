// Quest libr15.so VA 0x1457ed0; ReVault binary 576.
// ReVault raw parent 100227991, reconstruction node 100600442; grade M.
#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::CMemBlock {
void Resize(void* block, std::uint64_t bytes);
}

namespace NRadEngine::AudioPost {
namespace {

template <typename T>
T load(const std::byte* table, std::size_t offset) {
    T value{};
    std::memcpy(&value, table + offset, sizeof(value));
    return value;
}

template <typename T>
void store(std::byte* table, std::size_t offset, T value) {
    std::memcpy(table + offset, &value, sizeof(value));
}

}  // namespace

// @0x1457ed0 — CTable<CSimpleKey<SSound*, unsigned int>>::Insert
/* @confidence: M — container capacity and insertion behavior match the raw body; memory growth is delegated to the engine allocator. */
void InsertSoundKey(void* table_pointer, std::uint64_t index,
                    std::uint64_t insert_count, const SoundKey* value) {
    auto* table = static_cast<std::byte*>(table_pointer);
    const auto count = load<std::uint64_t>(table, 0x30);
    const auto capacity = load<std::uint64_t>(table, 0x28);
    const auto growth = load<std::uint64_t>(table, 0x20);
    const auto required = count + insert_count;

    if (required > capacity) {
        auto increment = capacity;
        if (increment < growth) increment = growth;
        if (increment < insert_count) increment = insert_count;
        NRadEngine::CMemBlock::Resize(
            table, (capacity + increment) * sizeof(SoundKey));
        store(table, 0x28, capacity + increment);
    }

    const auto entries = load<std::uintptr_t>(table, 0);
    const auto tail_count = count - index;
    if (tail_count != 0 && insert_count != 0) {
        std::memmove(reinterpret_cast<void*>(entries + (index + insert_count) * sizeof(SoundKey)),
                     reinterpret_cast<const void*>(entries + index * sizeof(SoundKey)),
                     tail_count * sizeof(SoundKey));
    }
    if (insert_count != 0) {
        for (std::uint64_t i = 0; i < insert_count; ++i) {
            std::memcpy(reinterpret_cast<void*>(entries + (index + i) * sizeof(SoundKey)),
                        value, sizeof(SoundKey));
        }
    }
    store(table, 0x30, required);
}

}  // namespace NRadEngine::AudioPost
