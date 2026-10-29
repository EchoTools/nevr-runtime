/* The CPU and GPU clock levels libr15 asks VrApi for (#928).
 *
 * What the game does (pinned libr15; pinned_targets.h has the address and the call contract): after
 * vrapi_EnterVrMode returns a session, CVR::UpdateTracking calls vrapi_SetClockLevels(session, cpu, gpu) once. The
 * levels are 5/5 when CGRenderConfig::StablePowerState() is true and 2/2 otherwise, so a shipping build asks for 2/2.
 * The call is once per entry into VR mode, not per frame.
 *
 * This module owns the decision, not the slot. GotHook allows one owner per libr15 slot (got_hook.h, kAlreadyInstalled), and
 * the render-configuration hook (render_config_hook.cpp, #1139) already owns the vrapi_SetClockLevels slot and is the one
 * place that sees the call. Its handler asks Choose() which levels to call the original with:
 *   - override on and the game asks for (2, 2): (4, 4);
 *   - any other pair, or override off: the game's own pair.
 * The original's result is returned as it came, and the render_config line reports the levels applied (cpu_level, gpu_level)
 * and VrApi's answer (clock_result), so a run says whether the headset granted what was asked.
 *
 * Choose() is lock-free and does not log, allocate or throw: it runs inside a hook (callback_thunk.h). Arm() logs one
 * `clock_levels_arm` line with the switch value, whatever it is; the switch is `diagnostics.clock_levels_override` in
 * config.yaml and defaults to off.
 *
 * Counters (hook_report.h), registered by RegisterCounters():
 *   clock_levels_calls               every call the game made
 *   clock_levels_overridden          the calls that were rewritten from (2, 2) to (4, 4)
 */
#pragma once

#include <cstdint>

namespace nevr_quest::clock_levels {

// What the shipping game asks for, and what the override asks for instead.
inline constexpr std::int32_t kGameCpuLevel = 2;
inline constexpr std::int32_t kGameGpuLevel = 2;
inline constexpr std::int32_t kOverrideCpuLevel = 4;
inline constexpr std::int32_t kOverrideGpuLevel = 4;

inline constexpr int kCounterCount = 2;

struct Levels {
  std::int32_t cpu;
  std::int32_t gpu;
};

// The levels vrapi_SetClockLevels is called with when the game asked for (cpu, gpu). Counts the call, and counts it as
// overridden when it was rewritten. Called from the render-configuration hook's handler.
Levels Choose(std::int32_t cpu, std::int32_t gpu) noexcept;

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one line
// {"event":"clock_levels_counters","result":"refused",...}, when any was refused.
bool RegisterCounters() noexcept;

// Sets the switch. One line {"event":"clock_levels_arm",...} says the value. Always true.
bool Arm(bool overrideOn) noexcept;

// Arm(overrideOn) when `countersRegistered`; otherwise logs the skip and leaves the switch off.
bool ArmIfCounted(bool countersRegistered, bool overrideOn) noexcept;

// Test support: sets the switch and clears the counters.
void ArmForTest(bool overrideOn) noexcept;
void ResetForTest() noexcept;

struct Counts {
  unsigned long long calls, overridden;
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::clock_levels
