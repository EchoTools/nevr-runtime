#pragma once

#include <cstdint>

/// The runtime's per-frame work dispatcher.
///
/// This is the single point at which plugin and module OnFrame callbacks are
/// driven, plus the periodic health and liveness work that has to happen on
/// *some* thread that actually runs. It lives in its own translation unit
/// because it is the most load-bearing function on a dedicated server and it
/// has nothing to do with the binary bug fixes.
///
/// There is exactly ONE of these, deliberately. A second copy could report the
/// wrong host type to every plugin on every client (N110). `just verify` fails
/// if a second one appears.
namespace nevr_frame {

/// Dispatch per-frame work. Safe to call from any hook on any thread, at any
/// rate: it is rate-limited to ~125 Hz internally and re-entrancy-guarded,
/// because OnFrame handlers may themselves call anything that calls back into
/// a hooked clock function.
///
/// `nowUs` is a monotonic microsecond timestamp. Pass 0 if no clock is
/// available; the call is then a no-op rather than a wrapped comparison.
void DispatchPerFrameWork(uint64_t nowUs);

/// The client's frame pacer (CPrecisionSleep::Wait hook) calls this on every frame: it records the calling thread as the
/// one that runs the game's frame. Work that touches the game's UI state runs only when `OnPacerThread()` is true, because
/// the dispatcher is also driven from GetTimeMicroseconds, which any thread may call. A server has no pacer: both stay
/// false and the work never runs there.
void NotePacerThread();
bool OnPacerThread();
unsigned long PacerThreadId();

}  // namespace nevr_frame
