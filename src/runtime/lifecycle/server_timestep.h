#pragma once

// #511: a dedicated server's fixed time step. Headless, the game paces its loop only on a fixed time
// step: the flag -fixedtimestep sets on the game object, and the step itself. Without the flag the
// loop runs as fast as it can; with it and the game's default step (33333 us) a server runs at 30
// ticks a second and the disc and players move in steps. A server gets the flag before the original
// PreprocessCommandLine (boot.cpp) and the step for g_headlessTickRateHz when the local config loads
// (config.cpp), as the legacy patch did. Pure: values and the two writes, through pointers the caller
// resolves, so a test can drive the server and client cases without the game (#515). Both call sites
// pass g_isServer; a client is left exactly as it was.
//
// Whether the step NEVR writes is still in the clock the server runs on is read back once, a second
// after the per-frame tick starts (ReadBackLine, config.cpp LogServerStepReadBack): two writers of the
// clock global (echovr.exe 0x1420a00e8) exist, and the later one resets the step to 33333 (addresses.h).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace nevr_server_timestep {

/// The game object's fixed-time-step flag (at GAME_TIMESTEP_FLAGS_OFFSET), what -fixedtimestep sets
/// (ApplyCommandLineEngineFlags ORs it into [pGame+0x828] at echovr.exe 0x1405045bf).
constexpr uint64_t kFixedTimestepFlag = 0x2000000;

/// The game's own step in microseconds: CClock::CClock stores it at echovr.exe 0x1400cd964
/// (`MOV qword ptr [RDI + 0x90], 0x8235`) and SetUseFixedTimeStep at 0x140511506. 1,000,000 / 30, rounded up.
constexpr uint64_t kGameDefaultStepUs = 33333;

/// `flags` with the fixed-time-step flag set; the other bits as they are.
inline uint64_t WithFixedTimestep(uint64_t flags) { return flags | kFixedTimestepFlag; }

/// The step for `ticksPerSecond`, in microseconds. 0 for a rate of 0: leave the game's step.
inline uint32_t StepMicroseconds(uint32_t ticksPerSecond) {
  return ticksPerSecond == 0 ? 0 : 1000000u / ticksPerSecond;
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
  kNotApplied = 0,     ///< ApplyStep has not run yet.
  kNotServer,          ///< A client: the game's step was left alone.
  kRateZero,           ///< The configured rate is 0: the game's step was left alone.
  kStateMissing,       ///< The game's time-step state was not there: nothing was written.
  kUnexpectedOldValue, ///< The field held neither the game's default step nor ours: nothing was written (N31).
  kSet,                ///< The step was written (or already was).
};

inline const char* OutcomeName(StepOutcome o) {
  switch (o) {
    case StepOutcome::kNotApplied: return "pending";
    case StepOutcome::kNotServer: return "not_server";
    case StepOutcome::kRateZero: return "rate_zero";
    case StepOutcome::kStateMissing: return "state_missing";
    case StepOutcome::kUnexpectedOldValue: return "unexpected_old_value";
    case StepOutcome::kSet: return "set";
  }
  return "unknown";
}

/// True when the step was meant to be written and was not: the server runs on the game's own step.
inline bool StepFailed(StepOutcome o) {
  return o == StepOutcome::kStateMissing || o == StepOutcome::kRateZero || o == StepOutcome::kUnexpectedOldValue;
}

/// The result of ApplyStep: the outcome, the step written (0 unless kSet) and the value found.
struct StepWrite {
  StepOutcome outcome = StepOutcome::kNotApplied;
  uint32_t stepUs = 0;
  uint64_t was = 0;
};

/// Writes the server's step, 1,000,000 / `rateHz` microseconds, to `*stepField` (the game's 64-bit step,
/// echovr.exe 0x140127a04 `MOV qword ptr [RBX + 0x90], RAX`) only when it holds the game's default step
/// (kGameDefaultStepUs) or already holds ours; anything else is a layout or version the code does not know
/// and nothing is written. A client, a rate of 0 or a null field writes nothing.
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
  if (w.was != kGameDefaultStepUs && w.was != step) {
    w.outcome = StepOutcome::kUnexpectedOldValue;
    return w;
  }
  *stepField = step;
  w.stepUs = step;
  w.outcome = StepOutcome::kSet;
  return w;
}

// --- the read-back --------------------------------------------------------------------------------------

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
