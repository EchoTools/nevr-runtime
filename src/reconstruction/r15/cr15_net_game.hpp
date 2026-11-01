#pragma once

#include <cstdint>

namespace NRadEngine::NRadGame {

struct SR15NetMatchSettings;

class CR15NetGame {
public:
    std::uint64_t PartyCount(std::uint16_t index) const;
    void FindMatch(SR15NetMatchSettings* settings);
};

}  // namespace NRadEngine::NRadGame
