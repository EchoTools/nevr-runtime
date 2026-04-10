#pragma once

#include <cstdint>
#include <vector>

namespace combat_mod {

/// Install hooks for InitModelCI and CNode3D::SetVisible to respect
/// the StartVisible flag on cloned actor models.
void InstallStartVisible(uintptr_t base, std::vector<void*>& hooks);

} // namespace combat_mod
