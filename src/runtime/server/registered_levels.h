#pragma once

// The level registrations a plugin made, as (on-disk type, name) pairs for AddRegisteredLevels (installed_levels.h):
// the plugin assets (NEVR_RegisterAsset) and the levels of the mounted resource sets (#2161). A set's level is
// listed only once its set is mounted, so a refused or not-yet-mounted set adds nothing to installed_levels.

#include <cstdint>
#include <utility>
#include <vector>

namespace nevr_game_server {

std::vector<std::pair<std::uint64_t, std::uint64_t>> RegisteredLevelKeys();

}  // namespace nevr_game_server
