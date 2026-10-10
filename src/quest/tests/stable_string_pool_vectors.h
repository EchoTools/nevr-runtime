#pragma once

#include <cstddef>

namespace nevr_quest_test {

struct StableStringVector {
  const char* value;
  std::size_t byteCountWithTerminator;
};

inline constexpr StableStringVector kStableStringVectors[] = {
    {"", 1},
    {"alpha", 6},
    {"beta", 5},
};

}  // namespace nevr_quest_test
