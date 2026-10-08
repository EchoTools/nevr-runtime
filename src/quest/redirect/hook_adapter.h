// Installs the CJson::TString thunks for the redirect (docs/adr/0003, "Config-string seam").
//
// This is the only redirect file that names GotHook, CallbackThunk and the pinned targets, so a
// change to that API (hook-backend work is still under review) lands here and nowhere else. The
// decision logic is in service_redirector.h and does not know the hook exists.
//
// Nothing installs unless the redirect feature is effective in the resolved configuration. A
// disabled feature, an unknown module or build ID, a slot that fails validation, or a refused
// pool leaves the game's original calls exactly as they were and logs one structured line per
// target (GotHook's own `got_hook` line plus this file's `redirect_install` line).
//
// libpnsradmatchmaking.so is not a dependency of libr15.so or libpnsrad.so and is loaded later
// (CNSLobby::LoadMatchmakingSupport), so its slot may not exist when InstallRedirectHooks runs.
// InstallRedirectHooks reports that as kModuleNotLoaded; the caller invokes
// InstallMatchmakingRedirect() once the module is loaded and before the first dial. Nothing here
// decides when that is.
#pragma once

#include "got_hook.h"
#include "quest/redirect/service_redirector.h"
#include "quest/sentinel/quest_config.h"

namespace nevr_quest::redirect {

struct InstallReport {
  bool featureEnabled = false;  // the redirect feature was effective
  sentinel::GotStatus libr15 = sentinel::GotStatus::kNotInstalled;
  sentinel::GotStatus matchmaking = sentinel::GotStatus::kNotInstalled;
};

// Where to hook. PinnedTargets() is the production value (build-ID and slot pinned).
struct HookTargets {
  sentinel::GotTarget libr15;
  sentinel::GotTarget matchmaking;
};
HookTargets PinnedTargets();

// Production entry: pinned targets, the process-wide pool, no bridge until SetBridgeProbe.
InstallReport InstallRedirectHooks(const nevr_quest::ResolvedConfig& config);

// Retries the matchmaking slot after its module has loaded. kNotInstalled if
// InstallRedirectHooks has not enabled the redirect.
sentinel::GotStatus InstallMatchmakingRedirect();

// Restores both slots and disarms the handlers. Pool strings already handed to the game stay valid.
void RemoveRedirectHooks();

// Test seam: every input InstallRedirectHooks fixes.
struct InstallOptions {
  HookTargets targets;
  sentinel::ImageLookup lookup;
  InternFn intern;
  BridgeProbe bridge;
};
InstallReport InstallRedirectHooksWith(const nevr_quest::ResolvedConfig& config, const InstallOptions& options);
sentinel::GotStatus InstallMatchmakingRedirectWith(sentinel::ImageLookup lookup);

// Test seam: arms the typed handlers on `redirector` (or disarms with nullptr) without touching
// any GOT slot. A test then calls each thunk's EntryAddress() with a fake original.
void ArmHandlersForTest(ServiceRedirector* redirector);

}  // namespace nevr_quest::redirect
