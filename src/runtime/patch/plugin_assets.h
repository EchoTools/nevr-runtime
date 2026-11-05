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

// #2161: installs the resolver hook (and the submit hook beside it) when they are not yet installed. True when the
// resolver hook (FillResourceFileRequest, 0x140fa1fe0) is in place, which the resource sets need.
bool EnsureResolverHook();

// #2161: the game's live manifest (*(resource manager + 8)), read with the game's own CPackageManifest::FindEntry
// (0x140f9eee0). StockRecordCount is 0 before the manifest exists; StockHas is false then.
uint64_t StockRecordCount();
bool StockHas(uint64_t type, uint64_t name);

#ifdef NEVR_TEST_HOOKS
// Test-only: installs one hook the way the engine hooks are installed, through PatchDetour, so HookGuard records it
// under `name`. On entry *original holds the game's function; on success it holds the trampoline.
bool InstallHookForTest(void* target, void* detour, void** original, const char* name);
// Test-only: retires one engine hook the way ShutdownPluginAssets does (PatchRetire, with the same status handling).
void RetireHookForTest(void* target, const char* what);
#endif

}  // namespace nevr::assets

extern "C" {

// Plugin ABI (v6). Return values are nevr::assets::RegisterStatus.
int NEVR_RegisterAsset(uint64_t type_hash, uint64_t name_hash, const void* data, uint64_t size,
                       const char* label);
void NEVR_DeregisterAssets(const void* data_start, const void* data_end);

}  // extern "C"
