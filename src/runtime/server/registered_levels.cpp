#include "runtime/server/registered_levels.h"

#include "runtime/patch/asset_registry.h"
#include "runtime/patch/resource_sets.h"
#include "runtime/server/installed_levels.h"

namespace nevr_game_server {

std::vector<std::pair<std::uint64_t, std::uint64_t>> RegisteredLevelKeys() {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> registered;
  for (const nevr::assets::Asset& asset : nevr::assets::Global().Snapshot()) {
    registered.emplace_back(asset.type, asset.name);
  }
  // A mounted set's levels: the mount checked each one's archive and everything that archive lists.
  for (const std::uint64_t level : nevr_resource_sets::MountedLevelNames()) {
    registered.emplace_back(kLevelRootType, level);
  }
  return registered;
}

}  // namespace nevr_game_server
