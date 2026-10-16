#pragma once

// #511: a dedicated server's fixed time step. Headless, the game paces its loop only on a fixed time
// step: the flag -fixedtimestep sets on the game object, and the step itself. Without the flag the
// loop runs as fast as it can; with it and the game's default step (33333 us) a server runs at 30
// ticks a second and the disc and players move in steps. A server gets the flag before the original
// PreprocessCommandLine (boot.cpp) and the step for g_headlessTickRateHz when the local config loads
// (config.cpp), as the legacy patch did. Pure: values and the two writes, through pointers the caller
// resolves, so a test can drive the server and client cases without the game (#515). Both call sites
// pass g_isServer; a client is left exactly as it was.

#include <cstdint>

namespace nevr_server_timestep {

/// The game object's fixed-time-step flag (at GAME_TIMESTEP_FLAGS_OFFSET), what -fixedtimestep sets
/// (ApplyCommandLineEngineFlags ORs it into [pGame+0x828] at echovr.exe 0x1405045bf).
constexpr uint64_t kFixedTimestepFlag = 0x2000000;

/// `flags` with the fixed-time-step flag set; the other bits as they are.
inline uint64_t WithFixedTimestep(uint64_t flags) { return flags | kFixedTimestepFlag; }

/// The step for `ticksPerSecond`, in microseconds. 0 for a rate of 0: leave the game's step.
inline uint32_t StepMicroseconds(uint32_t ticksPerSecond) {
  return ticksPerSecond == 0 ? 0 : 1000000u / ticksPerSecond;
}

/// Sets the fixed-time-step flag in `*flags` on a server. False (and `*flags` untouched) on a client or
/// when `flags` is null.
inline bool ApplyFlag(bool isServer, uint64_t* flags) {
  if (!isServer || flags == nullptr) return false;
  *flags = WithFixedTimestep(*flags);
  return true;
}

/// What ApplyStep did.
enum class StepOutcome : uint8_t {
  kNotApplied = 0,  ///< ApplyStep has not run yet.
  kNotServer,       ///< A client: the game's step was left alone.
  kRateZero,        ///< The configured rate is 0: the game's step was left alone.
  kStateMissing,    ///< The game's time-step state was not there: nothing was written.
  kSet,             ///< The step was written.
};

inline const char* OutcomeName(StepOutcome o) {
  switch (o) {
    case StepOutcome::kNotApplied: return "pending";
    case StepOutcome::kNotServer: return "not_server";
    case StepOutcome::kRateZero: return "rate_zero";
    case StepOutcome::kStateMissing: return "state_missing";
    case StepOutcome::kSet: return "set";
  }
  return "unknown";
}

/// The result of ApplyStep: the outcome, the step written (0 unless kSet) and the value it replaced.
struct StepWrite {
  StepOutcome outcome = StepOutcome::kNotApplied;
  uint32_t stepUs = 0;
  uint64_t was = 0;
};

/// Writes the server's step, 1,000,000 / `rateHz` microseconds, to `*stepField` (the game's 64-bit step,
/// echovr.exe 0x140127a04 `MOV qword ptr [RBX + 0x90], RAX`). A client, a rate of 0 or a null field writes
/// nothing.
inline StepWrite ApplyStep(bool isServer, uint32_t rateHz, uint64_t* stepField) {
  StepWrite w;
  if (!isServer) {
    w.outcome = StepOutcome::kNotServer;
    return w;
  }
  const uint32_t step = StepMicroseconds(rateHz);
  if (step == 0) {
    w.outcome = StepOutcome::kRateZero;
    return w;
  }
  if (stepField == nullptr) {
    w.outcome = StepOutcome::kStateMissing;
    return w;
  }
  w.was = *stepField;
  *stepField = step;
  w.stepUs = step;
  w.outcome = StepOutcome::kSet;
  return w;
}

}  // namespace nevr_server_timestep
