#pragma once

// #511: a dedicated server's fixed time step. Headless, the game paces its loop only on a fixed time
// step: the flag -fixedtimestep sets on the game object, and the step itself. Without the flag the
// loop runs as fast as it can; with it and the game's default step (33333 us) a server runs at 30
// ticks a second and the disc and players move in steps. A server gets the flag before the original
// PreprocessCommandLine (boot.cpp) and the step for g_headlessTickRateHz when the local config loads
// (config.cpp), as the legacy patch did. A server is always fixed-timestep; the rate is the config key
// network.tick_rate_hz (flat key nevr_tick_rate_hz, #595). Pure: values only, no game memory.

#include <cstddef>
#include <cstdint>
#include <cstdio>
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

/// `base + offset` as a pointer to a 64-bit field; null when `base` is null.
inline uint64_t* FieldAt(void* base, std::size_t offset) {
  if (base == nullptr) return nullptr;
  return reinterpret_cast<uint64_t*>(static_cast<char*>(base) + offset);
}

/// What ApplyFlag did.
enum class FlagOutcome : uint8_t {
  kNotApplied = 0,  ///< ApplyFlag has not run yet.
  kNotServer,       ///< A client: the flags were left alone.
  kFieldMissing,    ///< No flags field to write (the game object is not there): nothing was written.
  kSet,             ///< The flag was set.
};

inline const char* FlagOutcomeName(FlagOutcome o) {
  switch (o) {
    case FlagOutcome::kNotApplied: return "pending";
    case FlagOutcome::kNotServer: return "not_server";
    case FlagOutcome::kFieldMissing: return "field_missing";
    case FlagOutcome::kSet: return "set";
  }
  return "unknown";
}

/// Sets the fixed-time-step flag in `*flags` on a server. A client or a null `flags` writes nothing.
inline FlagOutcome ApplyFlag(bool isServer, uint64_t* flags) {
  if (!isServer) return FlagOutcome::kNotServer;
  if (flags == nullptr) return FlagOutcome::kFieldMissing;
  *flags = WithFixedTimestep(*flags);
  return FlagOutcome::kSet;
}

/// What ApplyStep did.
enum class StepOutcome : uint8_t {
  kNotApplied = 0,      ///< ApplyStep has not run yet.
  kNotServer,           ///< A client: the game's step was left alone.
  kKeepGameStep,        ///< The rate is 0: the game's own step is kept, by the operator's choice.
  kRateRefused,         ///< The rate is outside the allowed range: nothing was written.
  kStateMissing,        ///< The game's time-step state was not there: nothing was written.
  kUnexpectedOldValue,  ///< The slot held no step (version drift, N31): nothing was written.
  kWriteDidNotHold,     ///< The step was written and read back as something else.
  kSet,                 ///< The step was written and read back.
};

inline const char* OutcomeName(StepOutcome o) {
  switch (o) {
    case StepOutcome::kNotApplied: return "pending";
    case StepOutcome::kNotServer: return "not_server";
    case StepOutcome::kKeepGameStep: return "keep_game_step";
    case StepOutcome::kRateRefused: return "rate_refused";
    case StepOutcome::kStateMissing: return "state_missing";
    case StepOutcome::kUnexpectedOldValue: return "unexpected_old_value";
    case StepOutcome::kWriteDidNotHold: return "write_did_not_hold";
    case StepOutcome::kSet: return "set";
  }
  return "unknown";
}

/// True when the step was meant to be written and was not (or did not hold): the server runs on a step
/// other than the configured one by accident rather than by the operator's choice.
inline bool StepFailed(StepOutcome o) {
  return o == StepOutcome::kRateRefused || o == StepOutcome::kStateMissing ||
         o == StepOutcome::kUnexpectedOldValue || o == StepOutcome::kWriteDidNotHold;
}

/// The result of ApplyStep: the outcome, the step written (0 unless kSet) and the value found in the slot.
struct StepWrite {
  StepOutcome outcome = StepOutcome::kNotApplied;
  uint32_t stepUs = 0;
  uint64_t was = 0;
  uint64_t after = 0;  ///< The slot read back after a write.
};

/// Writes the server's step to `*stepField`, the game's 64-bit step (echovr.exe 0x140127a04
/// `MOV qword ptr [RBX + 0x90], RAX`), by PlanStepWrite: the rate is range-checked and the value found is
/// bounds-checked before the write (N31), and the write is read back. A client or a null field writes
/// nothing.
inline StepWrite ApplyStep(bool isServer, uint32_t rateHz, uint64_t* stepField) {
  StepWrite w;
  if (!isServer) {
    w.outcome = StepOutcome::kNotServer;
    return w;
  }
  if (stepField == nullptr) {
    w.outcome = StepOutcome::kStateMissing;
    return w;
  }
  volatile uint64_t* slot = stepField;
  w.was = *slot;
  const uint32_t current = w.was > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(w.was);
  const StepPlan plan = PlanStepWrite(rateHz, current);
  switch (plan.action) {
    case StepAction::KeepGameStep: w.outcome = StepOutcome::kKeepGameStep; return w;
    case StepAction::RefuseRate: w.outcome = StepOutcome::kRateRefused; return w;
    case StepAction::RefuseSlot: w.outcome = StepOutcome::kUnexpectedOldValue; return w;
    case StepAction::Write: break;
  }
  *slot = plan.stepUs;
  w.after = *slot;
  w.stepUs = plan.stepUs;
  w.outcome = (w.after <= 0xFFFFFFFFull && ReadBackMatches(plan.stepUs, static_cast<uint32_t>(w.after)))
                  ? StepOutcome::kSet
                  : StepOutcome::kWriteDidNotHold;
  return w;
}

// --- the live read-back ---------------------------------------------------------------------------------

/// Offsets within the clock object the global at echovr.exe 0x1420a00e8 points to (CClock): the step, and the
/// mode word whose bits 0x8 (fixed step) and 0x10 (paced) SetUseFixedTimeStep sets (0x1405114d0).
constexpr std::size_t kClockStepOffset = 0x90;
constexpr std::size_t kClockModeOffset = 0x50;
constexpr uint64_t kClockModeMask = 0x18;
/// Bytes of the clock object the read-back touches.
constexpr std::size_t kClockReadSpan = 0x98;

/// The per-frame tick (tick.cpp) on which a server reads the clock back, about a second after it starts.
constexpr long kReadBackTick = 125;

/// True on the one tick the read-back happens: a server, on tick kReadBackTick.
inline bool ShouldReadBack(bool isServer, long tick) { return isServer && tick == kReadBackTick; }

/// What the clock holds.
struct ClockReadBack {
  bool present = false;   ///< The clock pointer was non-null and its bytes readable.
  uint64_t stepUs = 0;
  uint64_t modeBits = 0;  ///< The mode word masked with kClockModeMask.
};

/// Reads the clock object at `state` (null: not present).
inline ClockReadBack ReadClock(const void* state) {
  ClockReadBack r;
  if (state == nullptr) return r;
  const char* base = static_cast<const char*>(state);
  r.present = true;
  r.stepUs = *reinterpret_cast<const uint64_t*>(base + kClockStepOffset);
  r.modeBits = *reinterpret_cast<const uint64_t*>(base + kClockModeOffset) & kClockModeMask;
  return r;
}

/// True when the clock holds `expectedStepUs`.
inline bool ReadBackIsLive(const ClockReadBack& rb, uint32_t expectedStepUs) {
  return rb.present && rb.stepUs == expectedStepUs && expectedStepUs != 0;
}

/// The one read-back log line. live=true when the clock holds `expectedStepUs`; `stepState` is the recorded
/// outcome of ApplyStep. The text names the clock pointer only as present or absent.
inline std::string ReadBackLine(const ClockReadBack& rb, uint32_t expectedStepUs, StepOutcome stepState,
                                long tick) {
  const bool live = ReadBackIsLive(rb, expectedStepUs);
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "server clock read-back tick=%ld clock=%s step_us=%llu mode_bits=0x%llx "
                "(0x8 fixed, 0x10 paced) expected_step_us=%u step_state=%s live=%s",
                tick, rb.present ? "present" : "absent", static_cast<unsigned long long>(rb.stepUs),
                static_cast<unsigned long long>(rb.modeBits), static_cast<unsigned>(expectedStepUs),
                OutcomeName(stepState), live ? "true" : "false");
  return buf;
}

}  // namespace nevr_server_timestep
