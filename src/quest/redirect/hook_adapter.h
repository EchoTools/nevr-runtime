// Installs the CJson::TString thunks for the redirect (docs/adr/0003, "Config-string seam").
//
// Installation and lifetime: GotHook, the redirector, the process-wide state. This file is built with
// exceptions. The thunks themselves (CallbackThunk, the pinned targets, the handler) are in
// tstring_thunks.{h,cpp}, built without, so a change to the thunk API lands there and here only
// through that header. The decision logic is in service_redirector.h and does not know the hook
// exists.
//
// Nothing installs unless the redirect feature is effective in the resolved configuration. A
// disabled feature, an unknown module or build ID, a slot that fails validation, or a refused
// pool leaves the game's original calls exactly as they were and logs one structured line per
// target (GotHook's own `got_hook` line plus this file's `redirect_install` line).
//
// The caller's contract (nothing in this directory calls these; the sentinel does):
//   1. RegisterRedirectCounters() before sentinel::StartReporter (the reporter refuses a
//      registration after it starts; 10 of the 32 counter slots are used).
//   2. InstallRedirectHooks(config) from the sentinel's ELF constructor, early enough to precede
//      CR15NetGame::Initialize, which reads config_host and configservice_host (libr15.so
//      0x1286060). A value the game read before the slot was hooked stays as the game parsed it. That
//      ordering rests on Bionic running the constructor before game code and is not tested on a
//      headset.
//   3. libpnsradmatchmaking.so is not a dependency of libr15.so or libpnsrad.so and is loaded
//      later (CNSLobby::LoadMatchmakingSupport), so its slot may not exist at step 2, which then
//      reports kModuleNotLoaded. The caller hooks libr15's dlopen slot (JUMP_SLOT 0x36c6380, the
//      string "pnsradmatchmaking") and calls InstallMatchmakingRedirect() after the real dlopen
//      returns. ConnectMatchmaker (0x1b22b8) re-reads the host on every connect, so installing
//      before the first dial is enough.
//   4. The first call with the redirect feature effective fixes the configuration, targets, pool and
//      bridge probe for the process; a later InstallRedirectHooks call with another configuration
//      is ignored (it only retries slots). A call with the feature off or without a pool installs
//      nothing and fixes nothing, so a later call can still activate. A failed install leaves the
//      redirector in place: retry libr15's slot with InstallLibR15Redirect() (or by calling
//      InstallRedirectHooks again) and the matchmaking slot with InstallMatchmakingRedirect().
//      A poisoned slot (kSlotPoisoned) is logged once and not retried.
// The bridge feature has no effect on Quest yet: the production install passes no BridgeProbe, so
// every redirect uses the configured target.
#pragma once

#include "got_hook.h"
#include "quest/redirect/service_redirector.h"
#include "quest/redirect/tstring_thunks.h"
#include "quest/sentinel/quest_config.h"

namespace nevr_quest::redirect {

struct InstallReport {
  bool featureEnabled = false;  // the redirect feature was effective
  sentinel::GotStatus libr15 = sentinel::GotStatus::kNotInstalled;
  sentinel::GotStatus matchmaking = sentinel::GotStatus::kNotInstalled;
};

// Registers every counter the redirect and its thunks keep (nothing here logs from a hooked call).
// Call before sentinel::StartReporter. False if a counter was refused.
bool RegisterRedirectCounters() noexcept;

// Production entry: pinned targets, the process-wide pool, no bridge probe.
InstallReport InstallRedirectHooks(const nevr_quest::ResolvedConfig& config);

// Retries libr15's slot with the options the first call fixed. kNotInstalled if the redirect was
// never enabled; kAlreadyInstalled if installed; kSlotPoisoned if an earlier attempt poisoned it.
sentinel::GotStatus InstallLibR15Redirect();

// Installs the matchmaking slot after its module has loaded (see the contract above). kNotInstalled
// if InstallRedirectHooks has not enabled the redirect; kAlreadyInstalled if it is installed;
// kSlotPoisoned if an earlier attempt left the slot poisoned.
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
sentinel::GotStatus InstallLibR15RedirectWith(sentinel::ImageLookup lookup);

// Test seam: arms the typed handlers on `redirector` (or disarms with nullptr) without touching
// any GOT slot. A test then calls each ThunkEntry(slot) with a fake original.
void ArmHandlersForTest(ServiceRedirector* redirector);

}  // namespace nevr_quest::redirect
