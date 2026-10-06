#pragma once

namespace nevr::lifecycle {

// Keep the game's exact result pointer when no redirect applies. The game uses
// pointer identity with defaultValue to recognize a missing JSON key.
inline const char* ChooseRedirectedOrOriginal(const char* originalResult,
                                               const char* redirectedResult) noexcept {
  return redirectedResult == nullptr ? originalResult : redirectedResult;
}

}  // namespace nevr::lifecycle
