#pragma once

// #511: a dedicated server's fixed time step. Headless, the game paces its loop only on a fixed time
// step: the flag -fixedtimestep sets on the game object, and the step itself. Without the flag the
// loop runs as fast as it can; with it and the game's default step (33333 us) a server runs at 30
// ticks a second and the disc and players move in steps. A server gets the flag before the original
// PreprocessCommandLine (boot.cpp) and the step for g_headlessTickRateHz when the local config loads
// (config.cpp), as the legacy patch did. A server is always fixed-timestep; the rate is the config key
// network.tick_rate_hz (flat key nevr_tick_rate_hz, #595). Pure: values only, no game memory.

#include <cstdint>
#include <string>

namespace nevr_server_timestep {

/// The game object's fixed-time-step flag (at GAME_TIMESTEP_FLAGS_OFFSET), what -fixedtimestep sets.
constexpr uint64_t kFixedTimestepFlag = 0x2000000;

/// `flags` with the fixed-time-step flag set; the other bits as they are.
inline uint64_t WithFixedTimestep(uint64_t flags) { return flags | kFixedTimestepFlag; }

/// The rate when the key is absent.
constexpr uint32_t kDefaultTickRateHz = 120;
/// The range of rates honoured. A rate of 0 is separate: it keeps the game's own step.
constexpr uint32_t kMinTickRateHz = 10;
constexpr uint32_t kMaxTickRateHz = 1000;
/// The game's own fixed step, 30 a second, in microseconds.
constexpr uint32_t kGameDefaultStepUs = 33333;

/// The step for `ticksPerSecond`, in microseconds. 0 for a rate of 0: leave the game's step.
inline uint32_t StepMicroseconds(uint32_t ticksPerSecond) {
  return ticksPerSecond == 0 ? 0 : 1000000u / ticksPerSecond;
}

/// The steps a step slot can plausibly hold: those of the rates in range. A value outside is not a
/// step, so the address it was read from is not the step slot (N31).
constexpr uint32_t kMinPlausibleStepUs = 1000000u / kMaxTickRateHz;
constexpr uint32_t kMaxPlausibleStepUs = 1000000u / kMinTickRateHz;

/// Parses network.tick_rate_hz. Absent or empty is kDefaultTickRateHz. 0 is 0: the game's own step,
/// still fixed (the flag is set for every server), never an unpaced loop. A value that is not a plain
/// whole number, or is outside [kMinTickRateHz, kMaxTickRateHz], is kDefaultTickRateHz with `problem`
/// set; it is never clamped or wrapped into a rate the operator did not write.
inline uint32_t ParseTickRateHz(const char* text, std::string* problem) {
  if (problem != nullptr) problem->clear();
  if (text == nullptr || text[0] == '\0') return kDefaultTickRateHz;
  uint64_t value = 0;
  for (const char* c = text; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9') {
      if (problem != nullptr) *problem = "not a whole number";
      return kDefaultTickRateHz;
    }
    value = value * 10 + static_cast<uint64_t>(*c - '0');
    if (value > kMaxTickRateHz) {
      if (problem != nullptr) *problem = "above the limit of " + std::to_string(kMaxTickRateHz);
      return kDefaultTickRateHz;
    }
  }
  if (value != 0 && value < kMinTickRateHz) {
    if (problem != nullptr) *problem = "below the limit of " + std::to_string(kMinTickRateHz);
    return kDefaultTickRateHz;
  }
  return static_cast<uint32_t>(value);
}

enum class StepAction {
  Write,         // write `stepUs` to the slot
  KeepGameStep,  // rate 0: the slot keeps the game's own step
  RefuseRate,    // the rate is outside the range: nothing is written
  RefuseSlot,    // the slot's current value is no step: the address is wrong, nothing is written
};

struct StepPlan {
  StepAction action;
  uint32_t stepUs;  // the step to write; meaningful for Write
};

/// What to do with the step slot for `hz`, given the `currentStepUs` read from it. The rate is
/// range-checked and the value read back is bounds-checked before anything is written.
inline StepPlan PlanStepWrite(uint32_t hz, uint32_t currentStepUs) {
  if (hz == 0) return {StepAction::KeepGameStep, 0};
  if (hz < kMinTickRateHz || hz > kMaxTickRateHz) return {StepAction::RefuseRate, 0};
  if (currentStepUs < kMinPlausibleStepUs || currentStepUs > kMaxPlausibleStepUs) {
    return {StepAction::RefuseSlot, 0};
  }
  return {StepAction::Write, StepMicroseconds(hz)};
}

/// Whether the slot holds the step that was written, read back after the write.
inline bool ReadBackMatches(uint32_t writtenStepUs, uint32_t readBackStepUs) {
  return writtenStepUs == readBackStepUs;
}

}  // namespace nevr_server_timestep
