#include "runtime/lifecycle/service_redirect.h"
#include "quest/tests/service_redirect_vectors.h"

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>

int main() {
  std::size_t index = 0;
  for (const auto& vector : nevr_quest_test::kRedirectVectors) {
    const auto actual = nevr_cfg::ResolveRedirect(
        vector.input,
        vector.socketTarget == nullptr ? std::nullopt
                                       : std::optional<std::string>(vector.socketTarget),
        vector.httpTarget == nullptr ? std::nullopt
                                     : std::optional<std::string>(vector.httpTarget),
        vector.bridgeActive, vector.bridgePort);
    if (vector.expected == nullptr) {
      if (actual.has_value()) {
        std::fprintf(stderr, "redirect vector %zu expected no replacement, got %s\n", index, actual->c_str());
        return 1;
      }
    } else {
      if (!actual.has_value() || *actual != vector.expected) {
        std::fprintf(stderr, "redirect vector %zu mismatch\n", index);
        return 1;
      }
    }
    ++index;
  }
  return 0;
}
