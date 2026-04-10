#pragma once

#include <cstdint>
#include <vector>

namespace combat_mod {

/// Install early hooks that must fire before level loads (team change tracking).
void InstallCombatEarly(uintptr_t base, std::vector<void*>& hooks);

/// Install level-gated combat hooks (respawn, net replication, script DLL discovery).
/// Called after mpl_arenacombat is detected.
void InstallCombatLevelHooks(uintptr_t base, std::vector<void*>& hooks);

/// Install sublevel offset hook (from swaptoggle).
void InstallLevelOffset(uintptr_t base, std::vector<void*>& hooks);

/// Per-frame update: F9 polling, script DLL discovery, pending offset application.
/// Returns true if combat mode is active.
bool CombatOnFrame();

/// Check if combat mode is currently active.
bool IsCombatActive();

} // namespace combat_mod
