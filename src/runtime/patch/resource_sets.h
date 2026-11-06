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

// Called once after the normal plugin pass: ends registration, installs the resolver hook and arms the mount, which the
// per-frame tick runs on the game's own thread once the game has asked the resolver for its first resource (OnFrame).
// With no set held it ends registration and nothing more.
void MountWhenReady();

// The resolver hook's note of each request: the first one tells the mount the game's boot package sets are all read.
void NoteRequest();

// Per frame, any thread; `onLoopThread` is true on the thread that runs the game's main wrapper (GameLoopThreadId).
// Runs an armed mount when it can, and writes one summary line per mounted set for each window in which the game read
// from it.
void OnFrame(bool onLoopThread);

// The resolver's question for one request made to `manager`: the shadow manager to fill it from when a mounted set
// holds (type, name) and `manager`'s manifest does not, else null (the game's own manifest answers). No lock, no
// allocation, no log line.
const void* ManagerFor(const void* manager, std::uint64_t type, std::uint64_t name);

// True when a set registered this run holds the resource a plugin asset keyed (type, name) would be: a set's record
// carries that key or an on-disk key of the asset's logical type (nevr_resource_key::OnDiskType, buffer 0 or 1). A
// plugin asset may not take it.
bool HoldsKey(std::uint64_t type, std::uint64_t name);

// The level names of the mounted sets, for the server registration's installed_levels.
std::vector<std::uint64_t> MountedLevelNames();

// The mounted sets as the login reports them: a JSON array of {plugin, label, levels, records, bytes}, "[]" when none.
std::string ReportJson();

#ifdef NEVR_TEST_HOOKS
// Test support: forget every set, the mount and the counters; the stock lookup Mount uses (null: the game's).
void ResetForTest();
using StockHasFn = bool (*)(std::uint64_t type, std::uint64_t name);
void SetStockForTest(StockHasFn has, std::uint64_t records);
// The millisecond clock the mount's wait reads (null: GetTickCount64).
void SetClockForTest(std::uint64_t (*now)());
// Makes the next registrations throw std::bad_alloc inside the host, before any check.
void SetThrowForTest(bool on);
// The file registration opens and compares instead of the plugin's own (null: the plugin's own).
void SetFilePathForTest(const wchar_t* path);
#endif

}  // namespace nevr_resource_sets
