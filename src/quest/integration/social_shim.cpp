// Built with -fno-exceptions (it includes social_install.h and login_prerequisites.h, which include callback_thunk.h).
#include "quest/integration/social_shim.h"

#include "quest/login/login_prerequisites.h"
#include "quest/social/social_install.h"

namespace nevr_quest::integration {

bool RegisterSocialCounters() noexcept { return quest_social::RegisterSocialReportCounters(); }

bool InstallSocialHook(const char** detail, bool presenceNames, bool presenceLocal) noexcept {
  // CNSOVRSocial's org-id lookups are refused once the Social() hook has selected the facade (#411).
  nevr_quest_login::local::SetSocialSelectedProbe(
      []() noexcept { return quest_social::Counters().selected.load(std::memory_order_relaxed) > 0; });
  quest_social::SetPresenceNames(presenceNames);
  quest_social::SetPresenceLocal(presenceLocal);
  const quest_social::InstallResult result = quest_social::InstallSocialHook(/*enabled=*/true);
  if (detail != nullptr) {
    *detail = result.status == quest_social::InstallStatus::kHookFailed ? sentinel::GotStatusName(result.got)
                                                                       : quest_social::InstallStatusName(result.status);
  }
  return result.status == quest_social::InstallStatus::kOk;
}

}  // namespace nevr_quest::integration
