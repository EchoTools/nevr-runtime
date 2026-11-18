// The core's view of the Quest's maps section (core/map_channel.h ReadSettings): the flat keys the core reads (service_map.cpp names them),
// answered from the sentinel's MapsConfig. Header-only, so a host test drives it without libcurl or the sentinel hooks that map_start.cpp
// links.
#pragma once

#include <string>
#include <vector>

#include "core/map_channel.h"
#include "quest/sentinel/quest_config.h"

namespace nevr_quest::maps {

/// The scalar keys: maps_base_url, maps_channel and maps_boot_budget_ms (a boot budget of 0 is "no limit", which the core reads from an
/// empty string). Any other key answers "".
inline nevr_map_channel::ScalarLookup ScalarsOf(const MapsConfig& config) {
  return [config](const char* key) -> std::string {
    if (std::string(key) == "maps_base_url") return config.baseUrl;
    if (std::string(key) == "maps_channel") return config.channel;
    if (std::string(key) == "maps_boot_budget_ms") return config.bootBudgetMs == 0 ? std::string() : std::to_string(config.bootBudgetMs);
    return std::string();
  };
}

/// The list keys: maps_official_keys and maps_trusted_keys. Any other key answers an empty list.
inline nevr_map_channel::ListLookup ListsOf(const MapsConfig& config) {
  return [config](const char* key) -> std::vector<std::string> {
    if (std::string(key) == "maps_official_keys") return config.officialKeys;
    if (std::string(key) == "maps_trusted_keys") return config.trustedKeys;
    return {};
  };
}

}  // namespace nevr_quest::maps
