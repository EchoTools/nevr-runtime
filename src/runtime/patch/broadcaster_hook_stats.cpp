#include "runtime/patch/broadcaster_hook_stats.h"

#include <cstdio>

namespace BroadcasterHookStats {

int Format(char* buffer, size_t buffer_size, long listen_entries, long dispatch_entries) {
  if (buffer == nullptr || buffer_size == 0) return 0;
  const int result = std::snprintf(
      buffer, buffer_size,
      "[NEVR.PATCH] broadcaster hook stats listen_entries=%ld dispatch_entries=%ld "
      "(N83/N84 evidence — zero entries means idle runs prove nothing)",
      listen_entries, dispatch_entries);
  return result < 0 ? 0 : result;
}

}  // namespace BroadcasterHookStats
