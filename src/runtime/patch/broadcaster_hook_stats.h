#pragma once

#include <cstddef>

namespace BroadcasterHookStats {

// Formats the N83/N84 liveness line into caller-owned storage.  This remains
// allocation-free because it is also emitted during shutdown diagnostics.
int Format(char* buffer, size_t buffer_size, long listen_entries, long dispatch_entries);

}  // namespace BroadcasterHookStats
