// Built with -fno-exceptions (it includes social_install.h, which includes callback_thunk.h).
#include "quest/integration/social_shim.h"

#include "quest/social/social_install.h"

namespace nevr_quest::integration {

bool RegisterSocialCounters() noexcept { return quest_social::RegisterSocialReportCounters(); }

bool InstallSocialHook(const char** detail, bool presenceNames, bool presenceLocal, bool uiEventProbe) noexcept {
  quest_social::SetPresenceNames(presenceNames);
  quest_social::SetPresenceLocal(presenceLocal);
  const quest_social::InstallResult result = quest_social::InstallSocialHook(/*enabled=*/true, uiEventProbe);
  if (detail != nullptr) {
    *detail = result.status == quest_social::InstallStatus::kHookFailed ? sentinel::GotStatusName(result.got)
                                                                       : quest_social::InstallStatusName(result.status);
  }
  return result.status == quest_social::InstallStatus::kOk;
}

}  // namespace nevr_quest::integration
