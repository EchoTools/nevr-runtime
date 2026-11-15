#pragma once

// The resource sets plugins carry (plugin API v9, #2161; docs/reference/plugin-resources.md), on PC: the
// register_resources entry plugins call from their init, the mount after the plugin pass, and the lookup the resolver
// hook (plugin_assets.cpp, Hook_Request) asks for each request. The checks and the shadow are resource_set_registry.h.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "runtime/patch/resource_set_registry.h"

namespace nevr_resource_sets {

// A downloaded map's set (docs/adr/0048-custom-maps-channel.md decision 5): the whole file the caller holds open as `held_handle`
// (a HANDLE, GENERIC_READ, shared for reading only, so the file cannot change while the game reads it), registered by the shared
// rules (core/resource_set_registry.h RegisterMapFile): the request's owner, label and pin, the re-hash when `request.rehash`, and
// the registry's check of the set (Registry::AddMapFile; an override is kOverride), with the same claim against plugin assets a
// plugin's set takes. The file is mapped read-only for the check only, never copied, and held committed for the mount. Call it
// before MountWhenReady: with no plugin set held, that mounts at once. On ok() the registry owns the handle for the process: the
// game's reader reads the file through it. On a refusal the caller keeps the handle. After the mount: kTooLate. One [NEVR.MAPS] line
// either way. With `request.rehash` it reads the whole file: run it off the game's thread, with no lock held.
Outcome RegisterFileResourceSet(const MapFileRequest& request, std::int64_t held_handle);

// Undoes RegisterFileResourceSet for `owner` before the mount (a map withdrawn whole, ADR 0048 decision 5): its set is dropped,
// committed or not, and its handle closed. Nothing after the mount, or for a plugin owner.
void WithdrawFileResourceSet(const std::string& owner);

// The mount observer (core/resource_set_registry.h MountObserver): called once a run, on the thread that ran the mount, right after
// Registry::Mount returned and before the first request is answered from the shadow. No lock of this file is held, so it may call
// MountedLevelNames and ReportJson. A registration that ends without a mount (the resolver hook missing, a mount that failed) calls
// it with an empty report. Default none: the plugin-only path is unchanged. An exception from it is logged and the mount goes on.
void SetMountObserver(void (*observer)(const MountReport& report));

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

// The lock a set's registration holds over its check of the plugin assets and its Add. A plugin asset's registration
// holds it over its check of the sets (HoldsKey) and its Register, so a set and an asset never both take one resource.
std::unique_lock<std::mutex> LockKeyClaims();

// True when a set registered this run holds the resource a plugin asset keyed (type, name) would be: a set's record
// carries that key or an on-disk key of the asset's logical type (nevr_resource_key::OnDiskType, buffer 0 or 1). A
// plugin asset may not take it.
bool HoldsKey(std::uint64_t type, std::uint64_t name);

// Text from outside (a plugin id from config.yaml, a plugin's label) as a log line carries it: printable ASCII but '"'
// and '\\', every other byte as '?', at most 64 bytes.
std::string Printable(const char* text);

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
// Makes the mount, or the read summary, throw std::bad_alloc at one place.
enum class MountThrowAt { kNowhere, kBeforeMount, kMountLines, kReadSummary };
void SetMountThrowForTest(MountThrowAt where);
// Makes MountWhenReady find the resolver hook not installed.
void SetNoResolverHookForTest(bool on);
// Makes the next registrations throw std::bad_alloc once the plugin's file is open.
void SetThrowAfterOpenForTest(bool on);
// Runs `hook` in each registration between its check of the plugin assets and its Add, under the key lock.
void SetBeforeAddForTest(void (*hook)());
// Runs `hook` at the mount right after the sets are published, before the frame's read summary.
void SetAfterPublishForTest(void (*hook)());
// The file registration opens and compares instead of the plugin's own (null: the plugin's own).
void SetFilePathForTest(const wchar_t* path);
// The registration's compare of the file with the mapped bytes, on `path`: 0 the same, 1 a read failed, 2 differs.
int FileCheckForTest(const wchar_t* path, std::uint64_t fileOffset, const unsigned char* mapped, std::uint64_t size);
#endif

}  // namespace nevr_resource_sets
