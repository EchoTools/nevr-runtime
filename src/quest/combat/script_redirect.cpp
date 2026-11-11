#include "quest/combat/script_redirect.h"

#include "quest/combat/script_redirect_list.h"
#include "quest/plugin/script_redirect.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::script_redirect {

namespace {
constexpr const char kOwner[] = "combat";
}  // namespace

ModuleResult Install(RegisterFn reg) noexcept {
  const NevrScriptRedirectResult result =
      reg != nullptr ? reg(kOwner, kHashes, kCount) : NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT;
  const bool ok = result == NEVR_SCRIPT_REDIRECT_OK;
  // The registry holds one registration per process: already_armed means another owner's is in force, so loads are redirected by that
  // one, and this module's list was not added. Every other refusal leaves nothing registered.
  const bool other = result == NEVR_SCRIPT_REDIRECT_ALREADY_ARMED;
  const char* const hostResult = ::nevr_quest::script_redirect::ResultName(result);
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : other ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kError,
                      "combat_script_redirect_install",
                      {{"result", ok ? "registered" : other ? "already_registered" : "refused"},
                       {"count", kCount},
                       {"host_result", hostResult},
                       {"msg", ok      ? "the game's loads of the nine combat script libraries open the APK's combat builds"
                               : other ? "another owner's redirect is already registered and stays in force; the combat list was not added"
                                       : "no combat script library is redirected: combat runs the stock scripts"}});
  return {ok, ok ? "registered" : hostResult};
}

ModuleResult Install() noexcept { return Install(&::nevr_quest::script_redirect::Register); }

}  // namespace nevr_quest::combat::script_redirect
