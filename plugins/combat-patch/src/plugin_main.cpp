/* SYNTHESIS -- custom tool code, not from binary */

#include "combat_patch.h"
#include "nevr_plugin_interface.h"
#include <cstdio>

static constexpr const char* PLUGIN_NAME = "combat_patch";
static constexpr const char* PLUGIN_DESC =
    "Enables combat weapons in arena mode by hooking game type checks, "
    "bypassing weapon stripping, and injecting weapon entity stubs.";

/* -- Plugin interface exports ----------------------------------------- */

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo() {
    return NvrPluginInfo{
        PLUGIN_NAME,
        PLUGIN_DESC,
        /* version */ 1, 0, 0
    };
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    if (!ctx) return -1;

    const char* config_path = "combat_patch_config.json";

    int result = nevr::combat_patch::Initialize(ctx->base_addr, config_path);
    if (result != 0) {
        std::fprintf(stderr, "[NEVR.COMBAT] Initialize failed: %d\n", result);
    }
    return result;
}

NEVR_PLUGIN_API void NvrPluginOnFrame(const NvrGameContext*) {
    nevr::combat_patch::OnFrame();
}

NEVR_PLUGIN_API void NvrPluginShutdown() {
    nevr::combat_patch::Shutdown();
}

/* -- DllMain for standalone injection --------------------------------- */

#ifdef _WIN32
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)hModule;
    (void)reserved;

    /*
     * Do NOT call Initialize/Shutdown here.
     * The plugin loader calls LoadLibraryA (which triggers DllMain) and then
     * calls NvrPluginInit separately.
     */
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#endif
