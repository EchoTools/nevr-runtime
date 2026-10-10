// Included by frames_probe_helper_ok.cpp (which defines NEVR_PROBE_ANNOTATED) and
// frames_probe_helper_bad.cpp. Built WITH exceptions.
#include <stdexcept>

#include "quest/sentinel/outside_game_call.h"

#ifdef NEVR_PROBE_ANNOTATED
#define NEVR_PROBE_MARK NEVR_OUTSIDE_GAME_CALL
#else
#define NEVR_PROBE_MARK __attribute__((noinline))
#endif

namespace probe {

NEVR_PROBE_MARK int LiveHelper(int value) noexcept {
  try {
    if (value == -12345) throw std::runtime_error("probe");
  } catch (const std::exception&) {
    return 0;
  }
  return value;
}

}  // namespace probe
