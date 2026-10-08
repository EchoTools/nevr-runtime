// The seam between sentinel/entry.cpp (built -fno-exceptions: it owns the clock hook's record) and the
// integration (built with exceptions). entry.cpp calls RunSentinelConstructor and defines the two clock
// functions; production_steps.cpp defines RunSentinelConstructor and calls the clock functions as steps.
#pragma once

namespace nevr_quest::integration {

// Defined in entry.cpp.
bool RegisterClockCounters() noexcept;  // 2 counters
bool InstallClockHook() noexcept;       // false when GotHook refused the slot (logged by GotHook)

// Defined in production_steps.cpp: the whole constructor sequence (ctor_sequence.h) over the real
// libraries. Never throws and never blocks.
void RunSentinelConstructor() noexcept;

}  // namespace nevr_quest::integration
