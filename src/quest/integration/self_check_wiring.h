#pragma once
// The Quest side of the self-checks (runtime/compat/self_check.h), kept apart from production_steps.cpp so a host
// test can drive it: the `self_check` feature's effect on the bridge's config and on the unit.

#include "quest/net/frame_tap.h"
#include "runtime/compat/self_check.h"

namespace nevr_quest::integration {

struct SelfCheckHooks {
  nevr_self_check::Sender sender = nullptr;  // hands one frame to the login connection (SessionBridge::SendToLogin)
  nevr_self_check::LogSink log = nullptr;    // one line in the sentinel's own log per result
  const char* build = "";                    // the build string every result carries
};

// Turns the unit on for this process and wires it into the bridge being configured: the login connection's
// upgrade asks for every remote log category (`*remoteDebugQuery`), the user the service names at LoginSuccess
// reaches the unit through `tap->onLoginUser` (an existing consumer is kept and called first), and the sender
// and log sink are set.
void ApplySelfCheck(quest_net::FrameTapSinks* tap, bool* remoteDebugQuery, const SelfCheckHooks& hooks);

// Self-check "matchmaking_reload_redirect" (#451, docs/engine/remote-log.md). The matchmaking redirect is a GOT
// hook installed once per process (post_load settles on the first install). A second mapping of
// libpnsradmatchmaking.so would not be covered, so the check compares the images the game's dlopen returned
// (post_load.h MatchmakingImages) with the installs. The probe is read on the self-check flush, never on the
// dlopen path; an image without an install is not a failure while the matchmaking action is still unsettled
// (not run yet, or retrying because the module was not mapped): it speaks once the action has settled.
void NoteMatchmakingRedirectInstalled();
bool MatchmakingReloadProbe(nevr_self_check::Observation* out);
// Test seam: forgets the install count and what the probe has reported.
void ResetMatchmakingReloadCheckForTest();

}  // namespace nevr_quest::integration
