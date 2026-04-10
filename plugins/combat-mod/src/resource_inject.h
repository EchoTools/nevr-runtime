#pragma once

#include <cstdint>
#include <vector>

namespace combat_mod {

/// Load combat override manifest and register file-based overrides.
/// Reads manifest.json from _overrides/combat/ next to the game binary.
/// Modified resources are registered with the gamepatches resource_override
/// system via NEVR_RegisterResourceOverride. Aliased resources (hash-swapped
/// copies) are recorded for the resource lookup hook.
///
/// Returns the number of resources loaded (modified + aliased).
int LoadCombatOverrides(uintptr_t base);

/// Check if a resource name hash should be aliased to the original combat
/// sublevel hash. Called from the resource lookup hook to redirect unmodified
/// resources to the original mpl_lobby_b_combat data in _data.
///
/// Returns true if name_hash is a combat sublevel resource that should be
/// loaded from the original hash instead. Sets *alias_hash to the original.
bool ShouldAliasResource(uint64_t type_hash, uint64_t name_hash,
                         uint64_t* alias_hash);

/// Deregister all combat overrides (call before plugin DLL unload).
void UnloadCombatOverrides();

} // namespace combat_mod
