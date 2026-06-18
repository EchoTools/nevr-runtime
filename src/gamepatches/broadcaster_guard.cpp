/* SYNTHESIS -- custom tool code, not from binary
 *
 * Broadcaster Guard — placeholder.
 * The ServerLib vtable shim in initialize.cpp handles the ABI
 * compatibility issue. This file remains for the future broadcaster
 * dispatch null-callback guard if needed.
 */

#include "broadcaster_guard.h"
#include <cstdio>

namespace BroadcasterGuard {

void Install(uintptr_t) {
    /* ServerLib shim installed via GetProcAddress hook in initialize.cpp */
}

void Shutdown() {}

} // namespace BroadcasterGuard
