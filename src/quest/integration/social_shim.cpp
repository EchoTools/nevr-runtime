// Built with -fno-exceptions (it includes social_install.h, which includes callback_thunk.h).
#include "quest/integration/social_shim.h"

#include "quest/social/social_install.h"

namespace nevr_quest::integration {

bool RegisterSocialCounters() noexcept { return quest_social::RegisterSocialReportCounters(); }

bool InstallSocialHook() noexcept {
  return quest_social::InstallSocialHook(/*enabled=*/true).status == quest_social::InstallStatus::kOk;
}

}  // namespace nevr_quest::integration
