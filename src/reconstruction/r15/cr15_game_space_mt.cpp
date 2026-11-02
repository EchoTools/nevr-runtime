// @0x18f8f24 — CncaGame::GameSpaceMT(CSymbol64) const
// Quest libr15.so, binary 576; raw parent 100255797; reconstruction node 100600329.
/* @confidence: H */

#include <cstddef>
#include <cstdint>

#include "cnca_game.hpp"

namespace q15t::r15::reconstruction {

void* CncaGame::GameSpaceMT(std::uint64_t symbol) const {
    const auto* const self = reinterpret_cast<const std::uint8_t*>(this);
    const auto find_sorted = [self, symbol](std::size_t count_offset,
                                             std::size_t entries_offset,
                                             bool* matched) -> void* {
        const auto count = *reinterpret_cast<const std::uint64_t*>(
            self + count_offset);
        const auto* const entries =
            *reinterpret_cast<const std::uint8_t* const*>(self + entries_offset);
        std::uint64_t low = 0;
        std::uint64_t high = count;
        while (low < high) {
            const std::uint64_t mid = low + (high - low) / 2;
            const auto* const entry = entries + mid * 0x10;
            const auto key = *reinterpret_cast<const std::int64_t*>(entry);
            const auto target = static_cast<std::int64_t>(symbol);
            if (key > target) {
                high = mid;
            } else if (key < target) {
                low = mid + 1;
            } else {
                *matched = true;
                return *reinterpret_cast<void* const*>(entry + 8);
            }
        }
        *matched = false;
        return nullptr;
    };

    bool matched = false;
    void* game_space = find_sorted(0x1f78, 0x1f48, &matched);
    if (matched && game_space != nullptr) {
        return game_space;
    }
    if (!matched) {
        game_space = find_sorted(0x1fb8, 0x1f88, &matched);
        if (matched && game_space != nullptr) {
            return game_space;
        }
    }

    const auto count = *reinterpret_cast<const std::uint64_t*>(self + 0x1f08);
    const auto* const spaces =
        *reinterpret_cast<const std::uint8_t* const*>(self + 0x1ed8);
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto* const space = *reinterpret_cast<void* const*>(
            spaces + index * sizeof(void*));
        const auto* const space_bytes =
            static_cast<const std::uint8_t*>(space);
        if (*reinterpret_cast<const std::uint64_t*>(space_bytes + 0x38) ==
            symbol) {
            return *reinterpret_cast<void* const*>(space_bytes + 0x30);
        }
    }
    return nullptr;
}

} // namespace q15t::r15::reconstruction
