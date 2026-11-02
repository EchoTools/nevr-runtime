// @0x18fe830 — CncaGameSpace::GameSpace(CSymbol64) const
// Quest libr15.so, binary 576; raw parent 100255902; reconstruction node 100600340.
/* @confidence: H */

#include <cstdint>

#include "cnca_game.hpp"

namespace q15t::r15::reconstruction {

void* CncaGameSpace::GameSpace(std::uint64_t symbol) const {
    const auto* const self = reinterpret_cast<const std::uint8_t*>(this);
    auto* const game = *reinterpret_cast<CncaGame* const*>(self + 8);
    return game->GameSpace(symbol);
}

} // namespace q15t::r15::reconstruction
