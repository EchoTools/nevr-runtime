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

// The trampoline must be visible to the detour before enabling the hook.
// Create returns false on failure; publish and enable are never called then.
template <typename Create, typename Publish, typename Enable>
bool CreatePublishEnable(Create&& create, Publish&& publish, Enable&& enable) {
  void* trampoline = nullptr;
  if (!create(&trampoline) || trampoline == nullptr) return false;
  publish(trampoline);
  return enable();
}

}  // namespace SocialFacade
