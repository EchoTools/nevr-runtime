#pragma once

// #511: a dedicated server's fixed time step. Headless, the game paces its loop only on a fixed time
// step: the flag -fixedtimestep sets on the game object, and the step itself. Without the flag the
// loop runs as fast as it can; with it and the game's default step (33333 us) a server runs at 30
// ticks a second and the disc and players move in steps. A server gets the flag before the original
// PreprocessCommandLine (boot.cpp) and the step for g_headlessTickRateHz when the local config loads
// (config.cpp), as the legacy patch did. Pure: values only, no game memory.

#include <cstdint>

namespace nevr_server_timestep {

/// The game object's fixed-time-step flag (at GAME_TIMESTEP_FLAGS_OFFSET), what -fixedtimestep sets.
constexpr uint64_t kFixedTimestepFlag = 0x2000000;

/// `flags` with the fixed-time-step flag set; the other bits as they are.
inline uint64_t WithFixedTimestep(uint64_t flags) { return flags | kFixedTimestepFlag; }

/// The step for `ticksPerSecond`, in microseconds. 0 for a rate of 0: leave the game's step.
inline uint32_t StepMicroseconds(uint32_t ticksPerSecond) {
  return ticksPerSecond == 0 ? 0 : 1000000u / ticksPerSecond;
}

}  // namespace nevr_server_timestep
