/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <vector>

namespace combat_mod {

/*
 * InstallLevelDetect -- hook the engine level load function to detect
 * mpl_arenacombat by hash.  Appends the hook target to `hooks` so the
 * caller can disable/remove them on shutdown.
 */
void InstallLevelDetect(uintptr_t base, std::vector<void*>& hooks);

/*
 * IsArenaCombat -- returns true once a level with the mpl_arenacombat
 * hash has been loaded.  Safe to call from any thread.
 */
bool IsArenaCombat();

} // namespace combat_mod
