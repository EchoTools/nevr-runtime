// Minimal assertion helpers for the Quest host tests (plain main, no framework:
// the same sources also have to build with the NDK, which has no GTest here).
#pragma once

#include <cstdio>

namespace quest_test {

inline int& Failures() {
  static int count = 0;
  return count;
}

}  // namespace quest_test

#define QCHECK(cond)                                                                   \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      ++quest_test::Failures();                                                        \
    }                                                                                  \
  } while (0)

// Compares two GotStatus values and names both on failure.
#define QCHECK_STATUS(actual, expected)                                                \
  do {                                                                                 \
    const auto qa = (actual);                                                          \
    const auto qe = (expected);                                                        \
    if (qa != qe) {                                                                    \
      std::fprintf(stderr, "%s:%d status %s, expected %s  [%s]\n", __FILE__, __LINE__, \
                   sentinel::GotStatusName(qa), sentinel::GotStatusName(qe), #actual); \
      ++quest_test::Failures();                                                        \
    }                                                                                  \
  } while (0)
