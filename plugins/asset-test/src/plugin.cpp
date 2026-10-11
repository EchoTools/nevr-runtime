/*
 * asset-test — proves plugin-carried assets (#432, plugin API v6).
 *
 * Carries a clone of a shipped level (the CGameLevelInfoResource and CGameLevelResource of one level, under a
 * new name) inside this DLL, registers both at init, then asks the engine's resource manager to load them
 * once, on the first frame. The log lines are the evidence:
 *
 *   host:   [NEVR.ASSET] registered ...            both assets accepted
 *   host:   [NEVR.ASSET] served from plugin ...    the engine read the bytes from this image
 *   plugin: [asset_test] load ... state=4          the engine finished initializing the resource
 *
 * The payload header is generated from a local install (tools/make_payload.py) and is not committed.
 */

#include "extension/plugin_interface.h"
#include "plugin_assets.h"
#include "plugin_logger.h"

#include <cstdint>

#include "asset_test_payload.h"

NEVR_DEFINE_PLUGIN_LOG("[asset_test]")

namespace {

// echovr.exe 34.4.631547: LoadResource(loader, &out, &{type, name}, 0, flags) and the loader global. The same
// call shape CArchiveResource's instance code uses (FUN_1401123d0).
constexpr uintptr_t kRvaLoadResource = 0xFA2820;
constexpr uintptr_t kRvaResourceLoader = 0x20A0708;
// DAT_1420a2ad8: the factory manager 0x140fa1c50 (resource-object creation) looks the resource type up in.
// The manager is created early (CncaGame::InitRenderWindowFromEngineFlags 0x1404f5870, from the game
// constructor) and is EMPTY until the factory installers run (0x1404f5bf0 -> 0x1403f1030 ... each
// CFactoryInstaller<T> constructor ends in CFactoryManager::InsertFactory 0x1401f3ee0). A LoadResource before
// the presence factory for its type exists faults inside 0x1401f3900: its first search
// (CFactoryManager_BinarySearch 0x1401f3e50) returns -1, the family entry becomes 0, and the second search
// dereferences 0x50 (observed in run 1: access violation at 0x1401f3e60, rcx=0x50).
//
// There is no single "registration complete" event to wait for, so the gate is the lookup 0x140fa1c50 itself
// performs, done with the same two searches and answered without dereferencing a missing entry:
//   family = search(manager, 0xa71a14664bd4e047)            // the presence-factory family
//   entry  = search(family + 0x50, <resource type symbol>)   // that type's factory
//   ready  = entry != -1 and the factory pointer at +0x58 is non-null
constexpr uintptr_t kRvaFactoryManager = 0x20A2AD8;
constexpr uintptr_t kRvaFactorySearch = 0x1F3E50;  // CFactoryManager_BinarySearch(list, key) -> index or -1
constexpr uint64_t kPresenceFamily = 0xa71a14664bd4e047ULL;
constexpr uint64_t kLoadFlags = 0x351127d67ULL;
constexpr size_t kObjState = 0x60;  // 4 = loaded and initialized

using LoadResourceFn = int64_t(__cdecl*)(void* loader, void** out, const uint64_t* key, uint64_t a, uint64_t flags);

uintptr_t g_base = 0;
bool g_registered = false;
uint64_t g_framesWaited = 0;
bool g_loadDone = false;

// The loader global is null until the engine has built its resource manager; try again on a later frame.
void* ResourceLoader() { return *reinterpret_cast<void**>(g_base + kRvaResourceLoader); }
using FactorySearchFn = int64_t(__cdecl*)(void* list, int64_t key);

// True when the presence factory for `type` is registered: the engine's own two searches, same order.
bool FactoryReady(uint64_t type) {
    void* manager = *reinterpret_cast<void**>(g_base + kRvaFactoryManager);
    if (manager == nullptr) return false;
    const auto search = reinterpret_cast<FactorySearchFn>(g_base + kRvaFactorySearch);
    const int64_t family = search(manager, static_cast<int64_t>(kPresenceFamily));
    if (family == -1) return false;
    auto* const* families = *reinterpret_cast<void* const* const*>(manager);
    auto* familyEntry = static_cast<uint8_t*>(families[family]);
    if (familyEntry == nullptr) return false;
    void* typeList = familyEntry + 0x50;
    const int64_t entry = search(typeList, static_cast<int64_t>(type));
    if (entry == -1) return false;
    auto* const* entries = *reinterpret_cast<void* const* const*>(typeList);
    auto* factoryEntry = static_cast<uint8_t*>(entries[entry]);
    return factoryEntry != nullptr && *reinterpret_cast<void**>(factoryEntry + 0x58) != nullptr;
}

void LoadOnce(const char* what, uint64_t type, uint64_t name) {
    auto load = reinterpret_cast<LoadResourceFn>(g_base + kRvaLoadResource);
    void* loader = ResourceLoader();
    const uint64_t key[2] = {type, name};
    void* resource = nullptr;
    const int64_t rc = load(loader, &resource, key, 0, kLoadFlags);
    int state = -1;
    if (resource) state = *reinterpret_cast<int*>(static_cast<uint8_t*>(resource) + kObjState);
    PluginLog("load %s type=0x%016llx name=0x%016llx rc=%lld state=%d", what,
              static_cast<unsigned long long>(type), static_cast<unsigned long long>(name),
              static_cast<long long>(rc), state);
}

}  // namespace

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "nevr-asset-test";
    info.description = "Carries a cloned level and loads it through the engine (#432)";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void) { return NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY; }

NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext* ctx, const char* /*args_json*/) {
    g_base = ctx ? ctx->base_addr : 0;
    const int info = nevr::RegisterAsset(kTypeLevelInfo, kLevelName, kLevelInfo, sizeof(kLevelInfo), "nevr_test_level.info");
    const int level = nevr::RegisterAsset(kTypeLevelResource, kLevelName, kLevelResource, sizeof(kLevelResource), "nevr_test_level.level");
    PluginLog("register info=%d level=%d (0 = ok)", info, level);
    g_registered = (info == NEVR_ASSET_OK && level == NEVR_ASSET_OK);
    return g_registered ? 0 : 1;
}

NEVR_PLUGIN_API void NvrPluginOnFrame(const NvrGameContext* /*ctx*/) {
    if (!g_registered || g_loadDone || g_base == 0) return;
    ++g_framesWaited;
    if (ResourceLoader() == nullptr) return;
    if (!FactoryReady(kTypeLevelInfo) || !FactoryReady(kTypeLevelResource)) return;
    PluginLog("factories ready for both level types after %llu frames",
              static_cast<unsigned long long>(g_framesWaited));
    g_loadDone = true;
    LoadOnce("level_info", kTypeLevelInfo, kLevelName);
    LoadOnce("level", kTypeLevelResource, kLevelName);
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
    // Each pointer registered above lies in this image; the range is the image of the function we are in.
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void*>(&NvrPluginShutdown), &mbi, sizeof(mbi)) != 0) {
        const auto* base = static_cast<const uint8_t*>(mbi.AllocationBase);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        nevr::DeregisterAssets(base, base + nt->OptionalHeader.SizeOfImage);
    }
}
