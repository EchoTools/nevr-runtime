// The seam between sentinel/entry.cpp (built -fno-exceptions: it owns the clock hook's record) and the
// integration (built with exceptions). entry.cpp calls RunSentinelConstructor and defines the two clock
// functions; production_steps.cpp defines RunSentinelConstructor and calls the clock functions as steps.
#pragma once

#include <cstddef>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/texture_quality.h"

namespace nevr_quest::integration {

// Defined in entry.cpp.
bool RegisterClockCounters() noexcept;  // 2 counters
bool InstallClockHook() noexcept;       // false when GotHook refused the slot (logged by GotHook)
// The #966 S7 frame dispatcher (quest/invite/frame_dispatch.h): the Update slot (login_prompt::InstallNetGameUpdate), taken on
// every launch whatever the login prompt does. Logs the one install line. True when the slot holds its thunk.
bool InstallFrameDispatchHook() noexcept;
// The #239 login-prompt hook (sentinel/login_prompt_hook.h): its counters and its install, through this
// seam because login_prompt_hook.h includes callback_thunk.h, which only -fno-exceptions units may.
bool RegisterLoginPromptCounters() noexcept;  // 17 counters (login_prompt::kCounterCount), and the title hook's 5 (#1137)
// login_prompt::InstallIfCounted(countersRegistered): installs only with its counters registered.
bool InstallLoginPromptHook(bool countersRegistered) noexcept;  // true when all three slots hold their thunks
// login_prompt::FlushCountersAtExit (#1391): writes the seventeen counters once. Called from the process-end callback,
// the android_main_returned point.
void FlushLoginPromptCountersAtExit() noexcept;

// The #1137 login-failed page title (sentinel/login_title_hook.h), through this seam for the same reason. Its counters are
// registered by RegisterLoginPromptCounters. The library holding the page's script is mapped after the constructor, so the
// install is a post-load action: kModuleNotLoaded means ask again after the next dlopen.
sentinel::GotStatus InstallLoginTitleHook() noexcept;

// The #319 OBB-mount skip (sentinel/obb_skip_hook.h), through this seam for the same reason.
bool RegisterObbSkipCounters() noexcept;  // 6 counters
// obb_skip::InstallIfCounted(countersRegistered): true when all three slots hold their thunks.
bool InstallObbSkipHook(bool countersRegistered) noexcept;

// The #943 texture-quality hook (sentinel/texture_quality_hook.h) and the #1012 Texture Resolution control
// (sentinel/texture_menu_hook.h), through this seam for the same reason: the eye-buffer size libr15 creates its swap chain
// with, and the settings-menu control that changes it while the game runs. Their counters are registered on every launch.
bool RegisterTextureQualityCounters() noexcept;  // 4 + 13 counters
// texture_quality::InstallIfCounted(level, fromConfig, countersRegistered), then texture_menu::Install(level, fromConfig): true
// when the eye-size slot holds the thunk and the control's five hooks hold theirs.
bool InstallTextureQualityHook(nevr_quest::texture_quality::Level level, bool fromConfig,
                               bool countersRegistered) noexcept;
// The #1139 render-configuration log (sentinel/render_config.h), through this seam for the same reason: five VrApi slots
// and a poller thread. No counters. `fromConfig`: whether the file gave the texture-quality level.
bool InstallRenderConfigHook(nevr_quest::texture_quality::Level level, bool fromConfig) noexcept;  // true when all five hold their thunks
// The #621 process-end hook (sentinel/process_end_hook.h), through this seam for the same reason: the process ends
// when the game's android_main returns. No counters. `SetProcessEndCallback` registers what runs on the hook's
// helper thread just before the process ends (the self-check report and flush); call it before the install.
void SetProcessEndCallback(void (*callback)() noexcept) noexcept;
bool InstallProcessEndHook() noexcept;  // true when the android_main slot holds the thunk

// The #1605 crash-first hook (sentinel/crash_first_hook.h), through this seam for the same reason: libr15's sigaction and
// signal slots keep the game's crash handler from displacing the sentinel's. No counters.
bool InstallCrashFirstHook() noexcept;  // true when both slots hold their thunks

// The #640 permission-wait hook (sentinel/permission_wait_hook.h), through this seam for the same reason: the game
// waits for the storage permission it just asked for. No counters.
bool InstallPermissionWaitHook() noexcept;  // true when the RequestPermissions slot holds the thunk

// The #868 Vulkan extension window (sentinel/vk_ext_window.h), through this seam for the same reason: the device extensions
// libr15 requires stay inside the 128 entries it reads. No counters.
bool InstallVkExtWindowHook() noexcept;  // true when the FindMethod and VrApi slots hold their thunks

// The #945 OS build floor (sentinel/os_build_gate_hook.h), through this seam for the same reason: the game never refuses a
// headset for its OS build number. No counters.
bool InstallOsBuildGateHook() noexcept;  // true when the OSBuildVersion slot holds the thunk

// The #871 resource request hook (sentinel/resource_request_hook.h; #1737 C5 adds the loose-file overlay), through this seam
// for the same reason: it records every FillResourceFileRequest call and serves a loose file on a manifest miss. Installed
// in every build; its per-request log lines are development builds only, a release build logs failed requests (the sequence asks nevr_build::kIsDev through
// ResourceRequestLogEnabled, defined in entry.cpp, which includes the generated nevr_build_info.h).
bool ResourceRequestLogEnabled() noexcept;          // nevr_build::kIsDev: whether the hook writes a line per request
bool RegisterResourceRequestCounters() noexcept;    // 4 counters
// resource_request::InstallCounted(countersRegistered, logLines): true when the slot holds the thunk.
bool InstallResourceRequestLogHook(bool countersRegistered, bool logLines) noexcept;
// The #928 clock-level override (sentinel/clock_levels_hook.h), through this seam for the same reason: the CPU and GPU levels
// libr15 asks VrApi for. It owns no slot; the render-configuration hook's handler asks it which levels to call with.
bool RegisterClockLevelsCounters() noexcept;  // 2 counters
// clock_levels::ArmIfCounted(countersRegistered, overrideOn): sets the switch only with its counters registered; true when
// armed. `overrideOn` is the switch (diagnostics.clock_levels_override).
bool ArmClockLevelsHook(bool countersRegistered, bool overrideOn) noexcept;

// The #966 main-menu cards (sentinel/menu_cards_hook.h), through this seam for the same reason: it includes
// callback_thunk.h. No counters. True when the callback slot holds the thunk.
bool InstallMenuCardsHook() noexcept;

// The #966 CREATE GAME / JOIN VIA CODE observer on libr15's MetricLog (invite/wizard_metric_hook.h, ADR 0017 slice S2), through
// this seam for the same reason. No counters. True when the observer's handler is in the game's call.
bool InstallWizardMetricHook() noexcept;

// The game's main level for the plugins (#1106, sentinel/main_level.h): the source the plugin loader reads and the frame work
// that keeps the plugins' contexts current. Called once, before the plugin plan; a refused frame work is logged there. No
// counters. True when the frame work is registered.
bool InstallPluginMainLevel() noexcept;

// The #1220 rad provider in pnsovr's place (sentinel/pns_provider_hook.h), through this seam for the same reason: it
// includes callback_thunk.h. No counters. A development build only (`PnsProviderEnabled` is nevr_build::kIsDev, defined in
// entry.cpp, which includes the generated nevr_build_info.h). True when all four libr15 slots hold their thunks.
bool PnsProviderEnabled() noexcept;
bool InstallPnsProviderHook() noexcept;

// Defined in production_steps.cpp: the whole constructor sequence (ctor_sequence.h) over the real
// libraries. Never throws and never blocks.
void RunSentinelConstructor() noexcept;

// Stops the bridge and joins the token-auth thread. Nothing calls it at process exit (the sentinel is
// never unloaded); it is the teardown path for tests and for a future unload.
void ShutdownIntegration() noexcept;

}  // namespace nevr_quest::integration
