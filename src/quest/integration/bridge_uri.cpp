#include "quest/integration/bridge_uri.h"

#include <cstdio>

namespace nevr_quest::integration {

std::string_view ReplaceBareBridgeUri(std::string_view value, unsigned port, const std::string& uri) noexcept {
  if (port == 0 || uri.empty()) return value;
  char bare[40];
  const int n = std::snprintf(bare, sizeof(bare), "ws://127.0.0.1:%u", port);
  if (n <= 0 || value != std::string_view(bare, static_cast<std::size_t>(n))) return value;
  return uri;
}

}  // namespace nevr_quest::integration
