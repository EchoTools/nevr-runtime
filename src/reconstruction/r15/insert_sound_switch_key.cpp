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

// @0x145aec4 — CTable<CSimpleKey<CSoundSymbol, CSoundSymbol>>::Insert
/* @confidence: M — capacity growth, element shift and insertion match the full stored CTable body and existing 16-byte-key instantiation. */
void InsertSoundSwitchKey(void* table_pointer, std::uint64_t index,
                          const SoundSwitchKey* value,
                          std::uint64_t insert_count) {
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
            table, (capacity + increment) * sizeof(SoundSwitchKey));
        store(table, 0x28, capacity + increment);
    }

    const auto entries = load<std::uintptr_t>(table, 0);
    const auto tail_count = count - index;
    if (tail_count != 0 && insert_count != 0) {
        std::memmove(
            reinterpret_cast<void*>(entries +
                                    (index + insert_count) * sizeof(SoundSwitchKey)),
            reinterpret_cast<const void*>(entries + index * sizeof(SoundSwitchKey)),
            tail_count * sizeof(SoundSwitchKey));
    }
    if (insert_count != 0) {
        for (std::uint64_t i = 0; i < insert_count; ++i) {
            std::memcpy(
                reinterpret_cast<void*>(entries +
                                        (index + i) * sizeof(SoundSwitchKey)),
                value, sizeof(SoundSwitchKey));
        }
    }
    store(table, 0x30, required);
}

}  // namespace NRadEngine::AudioPost
