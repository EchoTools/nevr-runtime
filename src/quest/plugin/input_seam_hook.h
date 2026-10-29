/* The controller-input seam's hook (#1202, docs/adr/0025-quest-autopilot-route.md): the Quest host service a plugin reaches
 * through ctx->quest->register_input_filter.
 *
 * One GOT hook, on libr15's vrapi_GetCurrentInputState slot (JUMP_SLOT 0x36ec0f0, build-ID pinned; pinned_targets.h). It
 * is installed at the first accepted registration and never before: a run whose config names no plugin that registers has
 * no hook on the slot. The handler calls the original with the game's arguments and returns what it returned. When the
 * call succeeded (0), a filter is registered and the state is a tracked controller's (type 4), it:
 *   - finds the device's hand (input_seam.h HandCache; on a miss, the capabilities through the function libr15's own
 *     vrapi_GetInputDeviceCapabilities slot holds, read once at registration and never written);
 *   - copies the six fields out (ReadState), calls the filter, and writes back only what it changed (WriteChanged).
 * The handler never logs, allocates or locks (callback_thunk.h rule 1). The filter runs after the original returned, so no
 * game frame is called from under it; its own rules are the ABI's (plugin_interface.h, NevrQuestInputFilterFn).
 *
 * Who may register: a development build only (the flag the plugin runtime is planned with, nevr_build::kIsDev), and one
 * filter per process, never removed. Each registration logs one line:
 *   {"event":"input_seam","result":"installed|refused","reason":"ok|bad_argument|not_dev_build|owned|caps_unresolved|
 *    hook_refused","plugin":"<id>","owned_by":"plugin:<id>","slot":"vrapi_GetCurrentInputState","status":"<GotStatus>"}
 * (`owned_by` only on `owned`, `status` only when a GOT install ran).
 *
 * input_seam_hook.cpp is built with -fno-exceptions (it reaches callback_thunk.h through pinned_targets.h). This header
 * includes neither, so the loader's exception-enabled translation units can include it.
 */
#pragma once

#include <cstdint>

#include "nevr/plugin_interface.h"
#include "quest/plugin/input_seam.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::input_seam {

// What a registration needs from the world. Production: the plugin runtime's development flag, InstallThunk on the
// pinned slot, and the value of libr15's pinned vrapi_GetInputDeviceCapabilities slot. A development environment has
// both functions; a release one is refused before either is called.
struct Environment {
  bool devBuild = false;
  sentinel::GotStatus (*install)() noexcept = nullptr;
  CapsFn (*resolveCaps)() noexcept = nullptr;
};

// The host API a Quest plugin gets in ctx->quest. A release build's API refuses every registration (not_dev_build).
// Process-lifetime.
const NevrQuestHostApi* HostApi(bool devBuild) noexcept;

// register_input_filter against `env`. Returns a NevrQuestFilterResult and logs the one line above.
std::int32_t RegisterWith(const Environment& env, const char* pluginId, NevrQuestInputFilterFn filter,
                          void* user) noexcept;

// The log token of a NevrQuestFilterResult ("ok", "bad_argument", ...); "unknown" for anything else.
const char* ResultName(std::int32_t result) noexcept;

// Test support: forgets the registration, the owner and the hand cache (the armed thunk then passes every call through).
// Not for production: a
// registered filter's code may still be running on the input thread.
void ResetForTest() noexcept;

}  // namespace nevr_quest::input_seam
