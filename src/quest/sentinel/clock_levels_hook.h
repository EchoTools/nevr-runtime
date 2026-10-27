/* The CPU and GPU clock levels libr15 asks VrApi for (#928).
 *
 * What the game does (pinned libr15; pinned_targets.h has the address and the call contract): after
 * vrapi_EnterVrMode returns a session, CVR::UpdateTracking calls vrapi_SetClockLevels(session, cpu, gpu) once. The
 * levels are 5/5 when CGRenderConfig::StablePowerState() is true and 2/2 otherwise, so a shipping build asks for 2/2.
 * The call is once per entry into VR mode, not per frame.
 *
 * One GOT hook on libr15's vrapi_SetClockLevels import, lock-free, on the caller's thread:
 *   - override on and the game asks for (2, 2): the original is called with (4, 4);
 *   - any other pair, or override off: the original is called with the game's own arguments.
 * The original's result is returned as it came.
 *
 * Whether the headset grants level 4 is not decided here: the original's result is logged beside the levels asked for and
 * the levels applied, so a run says what VrApi answered.
 *
 * Logging. The call is once per VR-mode entry, so the handler writes one `clock_levels` line per call (the first
 * kMaxLoggedCalls; later calls are counted and handled the same), after the original returned, built in a stack buffer
 * (hook_log.h):
 *   requested_cpu, requested_gpu   what the game passed
 *   applied_cpu, applied_gpu       what the original was called with
 *   result                         what the original returned
 *   override                       "on" or "off": the switch, `diagnostics.clock_levels_override` in config.yaml
 * and Install() writes one `clock_levels_install` line with the switch value and what was installed.
 *
 * Counters (hook_report.h), registered by RegisterCounters():
 *   clock_levels_calls               every call the game made
 *   clock_levels_overridden          the calls that were rewritten from (2, 2) to (4, 4)
 *   clock_levels_thunk_faults        the thunk had no original
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::clock_levels {

using Thunk = sentinel::pinned::LibR15SetClockLevelsThunk;

// What the shipping game asks for, and what the override asks for instead.
inline constexpr std::int32_t kGameCpuLevel = 2;
inline constexpr std::int32_t kGameGpuLevel = 2;
inline constexpr std::int32_t kOverrideCpuLevel = 4;
inline constexpr std::int32_t kOverrideGpuLevel = 4;

// `clock_levels` lines written; later calls are handled the same and not logged.
inline constexpr unsigned kMaxLoggedCalls = 64;

inline constexpr int kCounterCount = 3;

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one line
// {"event":"clock_levels_counters","result":"refused",...}, when any was refused.
bool RegisterCounters() noexcept;

// Sets the switch, arms the handler and installs the thunk in libr15.so. One line {"event":"clock_levels_install",...}
// says the switch and what was installed. True when the slot holds the thunk.
bool Install(bool overrideOn) noexcept;

// Install(overrideOn) when `countersRegistered`; otherwise logs the skip and installs nothing.
bool InstallIfCounted(bool countersRegistered, bool overrideOn) noexcept;

// Test support: arms the handler without touching a GOT slot, sets the switch, and clears the counters and the log count.
void ArmForTest(bool overrideOn) noexcept;
void ResetForTest() noexcept;

struct Counts {
  unsigned long long calls, overridden;
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::clock_levels
