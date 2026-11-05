#pragma once

// The resource sets plugins carry (plugin API v9, #2161; docs/reference/plugin-resources.md), on PC: the
// register_resources entry plugins call from their init, the mount after the plugin pass, and the lookup the resolver
// hook (plugin_assets.cpp, Hook_Request) asks for each request. The checks and the shadow are resource_set_registry.h.

#include <cstdint>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"

namespace nevr_resource_sets {

// The plugin loader brackets each plugin's init with these, so register_resources knows whose init is running, on
// which thread, and from which image. EndPluginInit keeps the plugin's sets for the mount when its init succeeded and
// drops them when it failed.
void BeginPluginInit(const char* pluginId, void* module);
void EndPluginInit(const char* pluginId, bool succeeded);

// ctx->register_resources (NevrGameContext, v9). Returns a NevrResourcesStatus.
int32_t HostRegisterResources(const char* pluginId, const NevrResourcesDesc* desc);

// Mounts the committed sets once the game's manifest exists. Called after the normal plugin pass; when the manifest is
// not built yet, the per-frame tick calls it again until it is.
void MountWhenReady();

// Per frame, any thread: a deferred mount, and one summary line per mounted set for each 10 s window in which the
// game read from it.
void OnFrame();

// The resolver's question for one request: the shadow manager to fill it from when a mounted set holds (type, name) and
// the game's manifests do not, else null (the game's own manifest answers). No lock, no allocation, no log line.
const void* ManagerFor(std::uint64_t type, std::uint64_t name);

// The level names of the mounted sets, for the server registration's installed_levels.
std::vector<std::uint64_t> MountedLevelNames();

// The mounted sets as the login reports them: a JSON array of {plugin, label, levels, records, bytes}, "[]" when none.
std::string ReportJson();

#ifdef NEVR_TEST_HOOKS
// Test support: forget every set, the mount and the counters; the stock lookup Mount uses (null: the game's).
void ResetForTest();
using StockHasFn = bool (*)(std::uint64_t type, std::uint64_t name);
void SetStockForTest(StockHasFn has, std::uint64_t records);
#endif

}  // namespace nevr_resource_sets
