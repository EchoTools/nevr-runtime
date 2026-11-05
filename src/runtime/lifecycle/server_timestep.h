#pragma once

// #511: a dedicated server's fixed time step. Headless, the game paces its loop only on a fixed time
// step: the flag -fixedtimestep sets on the game object, and the step itself. Without the flag the
// loop runs as fast as it can; with it and the game's default step (33333 us) a server runs at 30
// ticks a second and the disc and players move in steps. A server gets the flag before the original
// PreprocessCommandLine (boot.cpp) and the step for g_headlessTickRateHz when the local config loads
// (config.cpp), as the legacy patch did. A server is always fixed-timestep; the rate is the config key
// network.tick_rate_hz (flat key nevr_tick_rate_hz, #595).
//
// #2109: a server with no session ticks at kIdleTickRateHz. The boot write is the idle step; a session writes its own
// rate when it opens (ApplySessionTimestep, never leaving a session at the idle step: SessionFallbackRateHz,
// UntypedSessionStartWrites), and the idle rate goes back only when the game ends its session or the service ends the
// current one (IdlesAfterSessionEnd); a ServerDB reconnect does not end the game's match. The game paces each frame itself,
// in CClock::UpdateAndPace (echovr.exe 0x1400cf3b0): with the clock's mode bits 0x18 it waits out the rest of the step
// in CPrecisionSleep::Wait, which the runtime owns (patch/server_frame_wait.h answers UDP raw pings during that wait,
// so a slow idle tick does not delay them).
//
// What is pure: ParseTickRateHz, StepMicroseconds, PlanStepWrite, ReadBackMatches, WithFixedTimestep and the
// decision/format functions (DecideReadBack, ReadBackLine, OutcomeName) touch no memory but their arguments.
// ApplyFlag and ApplyStep write, and ApplyStep reads, the field a caller hands them; ReadClock reads the
// clock it is handed. They take the pointer from the caller (FieldAt resolves it), so a test drives the
// server and client cases on a buffer without the game (#515). StepRecord and FlagRecord hold an outcome
// for another thread to read. Both call sites pass g_isServer; a client is left exactly as it was.
//
// Whether the step NEVR writes is still in the clock the server runs on is read back ONCE, on per-frame tick
// kReadBackTick (about 1.2 s after the dispatcher starts): it says the step was live at that moment, not
// that it stays live. Two writers of the clock global (echovr.exe 0x1420a00e8) exist and the later one
// resets the step to 33333 (addresses.h, FIXED_TIMESTEP_PTR).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

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
/// The game's own rate, the one kGameDefaultStepUs is the step of.
constexpr uint32_t kGameTickRateHz = 30;

/// #2109: the rate a server ticks at while it has no session. It is below kMinTickRateHz on purpose: an operator
/// cannot configure it (ParseTickRateHz refuses it), only the idle write uses it. With no session there is no player to
/// simulate for; what still runs per frame is the network update (a session that opens is seen up to one idle step
/// later) and the per-frame work of an empty level. UDP raw pings do not wait for it: the frame wait answers them.
constexpr uint32_t kIdleTickRateHz = 6;

/// The step for `ticksPerSecond`, in microseconds. 0 for a rate of 0: leave the game's step.
inline uint32_t StepMicroseconds(uint32_t ticksPerSecond) {
  return ticksPerSecond == 0 ? 0 : 1000000u / ticksPerSecond;
}

/// #1785: the rate Combat's physics was tuned for, the game's own 30 a second (kGameDefaultStepUs is its step).
/// The game's body damping (echovr.exe 0x14064e790) is applied once per step with no factor of the step's length,
/// so a step 4x shorter damps a throw 4x as often in a second. Arena keeps kDefaultTickRateHz.
constexpr uint32_t kCombatTickRateHz = 30;

/// True for the Combat game types the community service sends (`echo_combat`, `echo_combat_private`,
/// `echo_combat_tournament`).
inline bool IsCombatGameType(std::string_view gameType) {
  constexpr std::string_view kPrefix = "echo_combat";
  return gameType.substr(0, kPrefix.size()) == kPrefix;
}

/// Where a session's tick rate came from.
enum class RateSource : uint8_t { kDefault, kCombatDefault, kConfig };

inline const char* RateSourceName(RateSource s) {
  switch (s) {
    case RateSource::kDefault: return "default";
    case RateSource::kCombatDefault: return "combat_default";
    case RateSource::kConfig: return "config.yaml";
  }
  return "unknown";
}

struct SessionRate {
  uint32_t hz = kDefaultTickRateHz;
  RateSource source = RateSource::kDefault;
};

/// The tick rate for a session of `gameType`. `configuredHz` is network.tick_rate_hz and `configExplicit` says the
/// operator wrote a valid value for it (an absent or invalid key is not explicit): an explicit value wins for every
/// game type, including 0 (the game's own step). Otherwise Combat gets kCombatTickRateHz and everything else,
/// Arena included, gets kDefaultTickRateHz.
inline SessionRate TickRateForGameType(std::string_view gameType, uint32_t configuredHz, bool configExplicit) {
  if (configExplicit) return {configuredHz, RateSource::kConfig};
  if (IsCombatGameType(gameType)) return {kCombatTickRateHz, RateSource::kCombatDefault};
  return {kDefaultTickRateHz, RateSource::kDefault};
}

/// The steps a step slot can plausibly hold: those of the rates in range and the idle rate's (#2109). A value outside
/// is not a step, so the address it was read from is not the step slot (N31).
constexpr uint32_t kMinPlausibleStepUs = 1000000u / kMaxTickRateHz;
constexpr uint32_t kMaxPlausibleStepUs = 1000000u / kIdleTickRateHz;
static_assert(kIdleTickRateHz < kMinTickRateHz, "the idle rate is below every rate an operator can configure");

/// #2109: the rate to write for a session's rate `hz`. 0 is the game's own rate: the slot holds the idle step while
/// no session runs, so a session at rate 0 writes the game's step back instead of leaving the slot alone.
inline uint32_t RateToWrite(uint32_t hz) { return hz == 0 ? kGameTickRateHz : hz; }

/// #2109: the step the idle write puts in the slot.
inline uint32_t IdleStepUs() { return 1000000u / kIdleTickRateHz; }

// --- when the server goes back to the idle step (#2109) ---------------------------------------------------------------

/// Why the runtime's record of a session ended.
enum class SessionEndCause : uint8_t {
  kGameEnded,     ///< The game ended its session: CNSLobby::EndSession (echovr.exe 0x140607e00) disconnected every peer
                  ///< (LeaveSession 0x14060b1f0), then called IServerLib::EndSession (vtable +0x38).
  kServiceEnded,  ///< ServerDB sent CODE_ENDED for a session id.
  kReconnect,     ///< The ServerDB socket reconnected: the runtime drops its record of the session and registers again.
                  ///< The game's match, if one runs, goes on (the socket drops during level loads, gameserver_callbacks.cpp).
  kShutdown,      ///< The process is exiting.
};

inline const char* SessionEndCauseName(SessionEndCause c) {
  switch (c) {
    case SessionEndCause::kGameEnded: return "game_ended";
    case SessionEndCause::kServiceEnded: return "service_ended";
    case SessionEndCause::kReconnect: return "reconnect";
    case SessionEndCause::kShutdown: return "shutdown";
  }
  return "unknown";
}

/// Whether a session end puts the server on the idle step. Only the end of the session the server is running does: the
/// game's own end (no peer is left to simulate for), or the service's CODE_ENDED naming the current session. A
/// reconnect ends only the runtime's record while the game's match goes on, a CODE_ENDED that names another session
/// (a late one, after a new session opened) is not this session's end, and a shutdown needs no step.
inline bool IdlesAfterSessionEnd(SessionEndCause cause, std::string_view endedSessionId,
                                 std::string_view currentSessionId) {
  switch (cause) {
    case SessionEndCause::kGameEnded: return true;
    case SessionEndCause::kServiceEnded: return !endedSessionId.empty() && endedSessionId == currentSessionId;
    case SessionEndCause::kReconnect:
    case SessionEndCause::kShutdown: return false;
  }
  return false;
}

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
/// range-checked (the configurable range, or the idle rate) and the value read back is bounds-checked before anything
/// is written.
inline StepPlan PlanStepWrite(uint32_t hz, uint32_t currentStepUs) {
  if (hz == 0) return {StepAction::KeepGameStep, 0};
  if (hz != kIdleTickRateHz && (hz < kMinTickRateHz || hz > kMaxTickRateHz)) return {StepAction::RefuseRate, 0};
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

/// #2109: whether the runtime's last write that held (`last`, from StepRecord) left the slot at the idle step.
inline bool LeftAtIdle(const StepWrite& last) {
  return last.outcome == StepOutcome::kSet && last.stepUs == IdleStepUs();
}

/// #2109: what the slot holds after a write that failed, as far as the write saw it: the read-back of a write that did
/// not hold, else the value found before the write. False when the slot could not be read at all (kStateMissing).
inline bool SlotAfterFailure(const StepWrite& failed, uint64_t* holds) {
  if (failed.outcome == StepOutcome::kStateMissing) return false;
  *holds = failed.outcome == StepOutcome::kWriteDidNotHold ? failed.after : failed.was;
  return true;
}

/// #2109: a session must never run at the idle step. When its own write failed and left the slot at the idle step, the
/// game's own rate is written instead (kGameTickRateHz, the step a server with no tick-rate write runs at). Returns
/// that rate, or 0 for no second write: the first write held or was the operator's choice (rate 0), the slot does not
/// hold the idle step (it holds no step, or another writer's value, so the session is not at 6 a second), or the slot
/// could not be read (a second write would fail the same way).
inline uint32_t SessionFallbackRateHz(const StepWrite& first) {
  if (!StepFailed(first.outcome)) return 0;
  uint64_t holds = 0;
  if (!SlotAfterFailure(first, &holds)) return 0;
  return holds == IdleStepUs() ? kGameTickRateHz : 0;
}

/// #2109: a session start whose frame names no game type (the legacy start frame alone, with no session create before
/// it) still opens a session, so it writes the rate for an unknown type when the slot holds the idle step. When the
/// session create already wrote the session's own rate, that rate stays: the frame cannot say better.
inline bool UntypedSessionStartWrites(const StepWrite& last) { return LeftAtIdle(last); }

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

/// #2109: what a session's write did: its own rate, and the fallback write SessionFallbackRateHz asked for, if any.
struct SessionStepWrite {
  StepWrite first;
  uint32_t fallbackHz = 0;  ///< 0: no second write was made
  StepWrite fallback;
};

/// #2109: writes a session's rate (RateToWrite of `sessionHz`) and, when that failed and left the slot at the idle
/// step, the game's own rate, so a session never runs at the idle step a write could have changed.
inline SessionStepWrite ApplySessionStep(bool isServer, uint32_t sessionHz, uint64_t* stepField) {
  SessionStepWrite r;
  r.first = ApplyStep(isServer, RateToWrite(sessionHz), stepField);
  r.fallbackHz = SessionFallbackRateHz(r.first);
  if (r.fallbackHz != 0) r.fallback = ApplyStep(isServer, r.fallbackHz, stepField);
  return r;
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

/// Reads the clock at `state` only when `readable` (the caller's check that its bytes are committed
/// and readable, N31); otherwise the clock is reported absent and nothing is dereferenced.
inline ClockReadBack ReadClockIf(bool readable, const void* state) {
  return ReadClock(readable ? state : nullptr);
}

/// What the read-back line says and at which level.
struct ReadBackDecision {
  uint32_t expectedStepUs = 0;
  bool live = false;      ///< The clock holds the expected step.
  std::string line;       ///< ReadBackLine for the expected step.
};

/// The step the clock should hold, and whether it does. After a write (kSet) it is the step written; at
/// rate 0 (kKeepGameStep) it is the game's own step the slot held (`w.was`), which is the operator's choice
/// and no failure; in every other state it is the configured rate's step. `live` is false for a clock that
/// is absent or holds anything else, and the caller logs a Warning for it.
inline ReadBackDecision DecideReadBack(const StepWrite& w, uint32_t rateHz, const ClockReadBack& rb, long tick) {
  ReadBackDecision d;
  if (w.outcome == StepOutcome::kSet) {
    d.expectedStepUs = w.stepUs;
  } else if (w.outcome == StepOutcome::kKeepGameStep) {
    d.expectedStepUs = w.was > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(w.was);
  } else {
    d.expectedStepUs = StepMicroseconds(rateHz);
  }
  d.live = ReadBackIsLive(rb, d.expectedStepUs);
  d.line = ReadBackLine(rb, d.expectedStepUs, w.outcome, tick);
  return d;
}

/// The outcome of ApplyStep, kept for the log lines that report it from another thread and another time.
class StepRecord {
 public:
  void Store(const StepWrite& w) {
    stepUs_.store(w.stepUs, std::memory_order_relaxed);
    was_.store(w.was, std::memory_order_relaxed);
    after_.store(w.after, std::memory_order_relaxed);
    outcome_.store(static_cast<uint8_t>(w.outcome), std::memory_order_release);
  }
  StepWrite Load() const {
    StepWrite w;
    w.outcome = static_cast<StepOutcome>(outcome_.load(std::memory_order_acquire));
    w.stepUs = stepUs_.load(std::memory_order_relaxed);
    w.was = was_.load(std::memory_order_relaxed);
    w.after = after_.load(std::memory_order_relaxed);
    return w;
  }

 private:
  std::atomic<uint32_t> stepUs_{0};
  std::atomic<uint64_t> was_{0};
  std::atomic<uint64_t> after_{0};
  std::atomic<uint8_t> outcome_{static_cast<uint8_t>(StepOutcome::kNotApplied)};
};

/// The outcome of ApplyFlag, kept the same way.
class FlagRecord {
 public:
  void Store(FlagOutcome o) { outcome_.store(static_cast<uint8_t>(o), std::memory_order_release); }
  FlagOutcome Load() const { return static_cast<FlagOutcome>(outcome_.load(std::memory_order_acquire)); }

 private:
  std::atomic<uint8_t> outcome_{static_cast<uint8_t>(FlagOutcome::kNotApplied)};
};


}  // namespace nevr_server_timestep
