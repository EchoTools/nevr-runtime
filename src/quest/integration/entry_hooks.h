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
bool RegisterLoginPromptCounters() noexcept;  // 14 counters
// login_prompt::InstallIfCounted(countersRegistered): installs only with its counters registered.
bool InstallLoginPromptHook(bool countersRegistered) noexcept;  // true when all three slots hold their thunks

// The #319 OBB-mount skip (sentinel/obb_skip_hook.h), through this seam for the same reason.
bool RegisterObbSkipCounters() noexcept;  // 4 counters
// obb_skip::InstallIfCounted(countersRegistered): true when both slots hold their thunks.
bool InstallObbSkipHook(bool countersRegistered) noexcept;

// The #621 process-end hook (sentinel/process_end_hook.h), through this seam for the same reason: the process ends
// when the game's android_main returns. No counters. `SetProcessEndCallback` registers what runs on the hook's
// helper thread just before the process ends (the self-check report and flush); call it before the install.
void SetProcessEndCallback(void (*callback)() noexcept) noexcept;
bool InstallProcessEndHook() noexcept;  // true when the android_main slot holds the thunk

// The #640 permission-wait hook (sentinel/permission_wait_hook.h), through this seam for the same reason: the game
// waits for the storage permission it just asked for. No counters.
bool InstallPermissionWaitHook() noexcept;  // true when the RequestPermissions slot holds the thunk

// Defined in production_steps.cpp: the whole constructor sequence (ctor_sequence.h) over the real
// libraries. Never throws and never blocks.
void RunSentinelConstructor() noexcept;

// Stops the bridge and joins the token-auth thread. Nothing calls it at process exit (the sentinel is
// never unloaded); it is the teardown path for tests and for a future unload.
void ShutdownIntegration() noexcept;

}  // namespace nevr_quest::integration
