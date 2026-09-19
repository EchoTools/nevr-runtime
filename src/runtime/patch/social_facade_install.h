#pragma once

#include <cstddef>

namespace SocialFacade {

enum class InstallStage { kBoot, kFacadeSelected };
enum class Probe { kAccessor, kSocialJson, kJsonSet, kJsonNavigateForWrite };

template <typename InstallOne>
void InstallHookPlan(InstallStage stage, bool enabled, InstallOne&& installOne) {
  if (stage == InstallStage::kBoot) {
    installOne(Probe::kAccessor);
    return;
  }
  if (!enabled) return;
  installOne(Probe::kSocialJson);
  installOne(Probe::kJsonSet);
  installOne(Probe::kJsonNavigateForWrite);
}

}  // namespace SocialFacade
