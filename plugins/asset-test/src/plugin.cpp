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
// DAT_1420a2ad8: the factory manager 0x140fa1c50 (resource-object creation) looks the resource type up in. The
// loader global above is set much earlier; a LoadResource before the factories are registered faults inside the
// factory lookup (observed: access violation at 0x1401f3e60 with rcx=0x50, run 1).
constexpr uintptr_t kRvaFactoryManager = 0x20A2AD8;
// Engine start-up is over well inside this; nothing else in this test knows when the factories are complete.
constexpr uint64_t kSettleMs = 20000;
constexpr uint64_t kLoadFlags = 0x351127d67ULL;
constexpr size_t kObjState = 0x60;  // 4 = loaded and initialized

using LoadResourceFn = int64_t(__cdecl*)(void* loader, void** out, const uint64_t* key, uint64_t a, uint64_t flags);

uintptr_t g_base = 0;
uint64_t g_initMs = 0;
bool g_registered = false;
bool g_loadDone = false;

// The loader global is null until the engine has built its resource manager; try again on a later frame.
void* ResourceLoader() { return *reinterpret_cast<void**>(g_base + kRvaResourceLoader); }
void* FactoryManager() { return *reinterpret_cast<void**>(g_base + kRvaFactoryManager); }

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
    g_initMs = GetTickCount64();
    const int info = nevr::RegisterAsset(kTypeLevelInfo, kLevelName, kLevelInfo, sizeof(kLevelInfo), "nevr_test_level.info");
    const int level = nevr::RegisterAsset(kTypeLevelResource, kLevelName, kLevelResource, sizeof(kLevelResource), "nevr_test_level.level");
    PluginLog("register info=%d level=%d (0 = ok)", info, level);
    g_registered = (info == NEVR_ASSET_OK && level == NEVR_ASSET_OK);
    return g_registered ? 0 : 1;
}

NEVR_PLUGIN_API void NvrPluginOnFrame(const NvrGameContext* /*ctx*/) {
    if (!g_registered || g_loadDone || g_base == 0) return;
    if (GetTickCount64() - g_initMs < kSettleMs) return;
    if (ResourceLoader() == nullptr || FactoryManager() == nullptr) return;
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
