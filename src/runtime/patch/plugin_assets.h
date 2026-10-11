#pragma once

// Plugin-carried assets: levels and resources a plugin ships inside its own image, served to the engine's
// resource manager from the plugin's memory. See asset_registry.h for the bookkeeping and docs/reference/
// plugin-assets.md for the contract plugin authors build against.

#include <cstddef>
#include <cstdint>

#include "runtime/patch/asset_registry.h"

namespace nevr::assets {

// Register an asset the plugin carries. Installs the engine hooks on first use. Returns why it was refused;
// every refusal is also a Warning in the log naming the label, type and name.
RegisterStatus RegisterPluginAsset(uint64_t type, uint64_t name, const void* data, uint64_t size,
                                   const char* label);

// Remove every asset whose data lies in [start, end). Plugins call this from their shutdown with their own
// image range; the host also calls it with the image range of each plugin it unloads.
size_t DeregisterPluginAssets(const void* start, const void* end);

// Remove the hooks and drop the registry. Idempotent.
void ShutdownPluginAssets();

}  // namespace nevr::assets

extern "C" {

// Plugin ABI (v6). Return values are nevr::assets::RegisterStatus.
int NEVR_RegisterAsset(uint64_t type_hash, uint64_t name_hash, const void* data, uint64_t size,
                       const char* label);
void NEVR_DeregisterAssets(const void* data_start, const void* data_end);

}  // extern "C"
