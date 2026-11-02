// @0x18f8ab8 — CncaGame::LoadedGameSpace(CSymbol64) const
// Quest libr15.so, binary 576; raw parent 100255785; reconstruction node 100600338.
/* @confidence: M */

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "cnca_game.hpp"

namespace q15t::r15::reconstruction {

struct GameSpaceEntry {
    std::int64_t symbol;
    void* game_space;
};

static void SortAndSearchTable(const std::uint8_t* self,
                               std::size_t count_offset,
                               std::size_t entries_offset,
                               std::size_t flag_offset,
                               std::uint64_t symbol,
                               void** result,
                               bool* matched) {
    const auto count = *reinterpret_cast<const std::uint64_t*>(self + count_offset);
    auto* entries = *reinterpret_cast<GameSpaceEntry* const*>(self + entries_offset);
    auto* const flags = reinterpret_cast<std::uint32_t*>(
        const_cast<std::uint8_t*>(self + flag_offset));

    if (count != 0 && (*flags & 1U) != 0) {
        std::sort(entries, entries + count,
                  [](const GameSpaceEntry& left, const GameSpaceEntry& right) {
                      return left.symbol < right.symbol;
                  });
        *flags &= ~1U;
    }

    std::uint64_t low = 0;
    std::uint64_t high = count;
    const auto key = static_cast<std::int64_t>(symbol);
    while (low < high) {
        const std::uint64_t middle = low + (high - low) / 2;
        if (entries[middle].symbol < key) {
            low = middle + 1;
        } else if (entries[middle].symbol > key) {
            high = middle;
        } else {
            *result = entries[middle].game_space;
            *matched = true;
            return;
        }
    }
    *matched = false;
}

void* CncaGame::LoadedGameSpace(std::uint64_t symbol) const {
    const auto* const self = reinterpret_cast<const std::uint8_t*>(this);
    void* result = nullptr;
    bool matched = false;
    SortAndSearchTable(self, 0x1f78, 0x1f48, 0x1f80, symbol, &result, &matched);
    if (matched) {
        return result;
    }

    SortAndSearchTable(self, 0x1fb8, 0x1f88, 0x1fc0, symbol, &result, &matched);
    return matched ? result : nullptr;
}

} // namespace q15t::r15::reconstruction
