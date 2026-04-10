#pragma once
#include "hook_manager.h"

#include <cstdint>
#include <vector>

namespace combat_mod {

/// Install hooks for InitModelCI and CNode3D::SetVisible to respect
/// the StartVisible flag on cloned actor models.
void InstallStartVisible(uintptr_t base, nevr::HookManager& hooks);

} // namespace combat_mod
