// @0x18d3414 — CncaGame::GameSpace(CSymbol64) const
// Quest libr15.so, binary 576; raw parent 100255110; reconstruction node 100600339.
/* @confidence: H */

#include <cstddef>
#include <cstdint>

#include "cnca_game.hpp"

namespace q15t::r15::reconstruction {

void* CncaGame::GameSpace(std::uint64_t symbol) const {
    if (void* const loaded = LoadedGameSpace(symbol)) {
        return loaded;
    }

    const auto* const self = reinterpret_cast<const std::uint8_t*>(this);
    const auto count = *reinterpret_cast<const std::uint64_t*>(self + 0x1f08);
    const auto* const spaces =
        *reinterpret_cast<void* const* const*>(self + 0x1ed8);
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto* const space =
            static_cast<const std::uint8_t*>(spaces[index]);
        if (*reinterpret_cast<const std::uint64_t*>(space + 0x38) == symbol) {
            return *reinterpret_cast<void* const*>(space + 0x30);
        }
    }
    return nullptr;
}

} // namespace q15t::r15::reconstruction
