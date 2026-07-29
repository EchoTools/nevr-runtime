/* SYNTHESIS -- custom tool code, not from binary
 *
 * Broadcaster Guard — placeholder.
 * The ServerLib vtable shim in initialize.cpp handles the ABI
 * compatibility issue. This file remains for the future broadcaster
 * dispatch null-callback guard if needed.
 */

#include "runtime/patch/broadcaster_guard.h"
#include <cstdio>

namespace BroadcasterGuard {

void Install(uintptr_t) {
    /* Deliberately empty. The ServerLib ABI shim lives in initialize.cpp's
     * GetProcAddress hook; this file is a reserved seam for a future broadcaster
     * dispatch guard.
     *
     * If you are an agent looking for the broadcaster dispatch guard because a
     * boot log said one was installed: it never existed. That log line was wrong
     * and is now corrected at its call site (initialize.cpp). An empty function
     * whose caller announces success is indistinguishable from a working one in
     * every artifact except this file — which is why the announcement, not the
     * emptiness, was the defect.
     *
     * The real dispatch-integrity work is N83/N84: see
     * docs/primers/2026-07-26-n83-broadcaster-severance.md. */
}

void Shutdown() {}

} // namespace BroadcasterGuard
