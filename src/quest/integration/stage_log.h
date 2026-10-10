// Stable stage names for the smoke-test logs. A failed run must say WHERE it stopped without another
// build, so every stage of the integration emits one structured line whose `event` is a fixed name and
// whose `status` / `class` say how it went:
//
//   config_loaded            configuration resolved; the effective feature switches
//   clock_hook_installed     the always-on proof hook
//   token_auth_state         the token session's readiness (launched, then every change)
//   login_prompt_hook_installed  the sign-in prompt hook on libr15's SetDelimitedErrorMessage slot (#239)
//   router_listening         the loopback listener is bound (port)
//   redirect_installed       the CJson::TString thunk on libr15
//   social_hook_installed    the social facade on libr15's CNSProvider::Social slot
//   dlopen_hook_installed    the hook on libr15's dlopen slot
//   libpnsovr_loaded         the game's dlopen returned libpnsovr.so
//   login_hook_installed     the login hook on libpnsovr
//   matchmaking_redirect_installed   the TString thunk on libpnsradmatchmaking
//   router_remote_connected  a remote WebSocket session to the NEVR service opened
//   router_remote_failed     a remote session could not open (class = the reason)
//   login_rewritten          the game's login was rewritten into the NEVR login (or why not)
//   login_accepted           the service answered LoginSuccess
//   login_refused            the service answered LoginFailure
//   social_facade_selected   reporter counter `social_facade_selected` (first_change), not a stage line
//
// status is one of ok, failed, skipped (or a short fixed state token); class is a fixed token that
// names the failure (a GotStatus name, a reason from the sequence), never a value.
#pragma once

#include <optional>
#include <string_view>

namespace nevr_quest::integration {

// The stage a constructor-sequence step reports under; nullptr for a step with no stage line.
const char* StageForStep(const char* stepName) noexcept;

// The level a step's outcome is logged at. ok, and a skip because the feature is off (`*_off`,
// `nothing_to_install_after_load`), are Info; a skip because a dependency is unavailable is Warn; a skip
// because the step's counters were refused, and a failed or thrown step, are Error, so a hook that silently
// stayed out (for example social after `register_social_counters` was refused) is an error line.
enum class StepLevel { kInfo, kWarn, kError };
StepLevel StepLogLevel(const char* state, const char* reason) noexcept;

struct StageEvent {
  const char* event;   // a stage name above
  const char* status;  // ok | failed
  const char* cls;     // class token
};

// Maps a line of the router / remote transport / bridge log to a stage event, if it is one. Matches the
// fixed prefixes those components write; never reads a value out of the line.
std::optional<StageEvent> ClassifyRouterLine(std::string_view line) noexcept;

}  // namespace nevr_quest::integration
