// The seam between sentinel/entry.cpp (built -fno-exceptions: it owns the clock hook's record) and the
// integration (built with exceptions). entry.cpp calls RunSentinelConstructor and defines the two clock
// functions; production_steps.cpp defines RunSentinelConstructor and calls the clock functions as steps.
#pragma once

namespace nevr_quest::integration {

// Defined in entry.cpp.
bool RegisterClockCounters() noexcept;  // 2 counters
bool InstallClockHook() noexcept;       // false when GotHook refused the slot (logged by GotHook)
// The #239 login-prompt hook (sentinel/login_prompt_hook.h): its counters and its install, through this
// seam because login_prompt_hook.h includes callback_thunk.h, which only -fno-exceptions units may.
bool RegisterLoginPromptCounters() noexcept;  // 3 counters
bool InstallLoginPromptHook() noexcept;       // false when GotHook refused the slot (logged by GotHook)

// Defined in production_steps.cpp: the whole constructor sequence (ctor_sequence.h) over the real
// libraries. Never throws and never blocks.
void RunSentinelConstructor() noexcept;

// Stops the bridge and joins the token-auth thread. Nothing calls it at process exit (the sentinel is
// never unloaded); it is the teardown path for tests and for a future unload.
void ShutdownIntegration() noexcept;

}  // namespace nevr_quest::integration
