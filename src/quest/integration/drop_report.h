// The router drops an STcpConnectionUnrequireEvent on purpose when the connection it belongs to has nothing
// outstanding (delivering it would wrap the game's 8-bit count). The count is Router Stats; production reports
// it from the token-auth poll as one structured line each time it changed, so a dropped Unrequire is visible
// without a line per frame. (The reporter's counter table is nearly full: 46 of 48.)
#pragma once

#include <cstdint>

namespace nevr_quest::integration {

// True when `current` differs from what was last reported; `*last` then holds `current` and `*delta` the
// increase since the previous report.
inline bool DropsChanged(std::uint64_t current, std::uint64_t* last, std::uint64_t* delta) noexcept {
  if (current == *last) return false;
  *delta = current > *last ? current - *last : current;  // a counter that went back (a new router) reports itself
  *last = current;
  return true;
}

}  // namespace nevr_quest::integration
