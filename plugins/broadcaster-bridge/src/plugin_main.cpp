/* SYNTHESIS -- custom tool code, not from binary */

#include "broadcaster_bridge.h"
#include "nevr_plugin_interface.h"
#include <cstdio>

static constexpr const char* PLUGIN_NAME = "broadcaster_bridge";
static constexpr const char* PLUGIN_DESC =
    "Mirrors CBroadcaster_Send packets to a UDP debug target and accepts "
    "injected packets for testing.";

/* ── Plugin interface exports ─────────────────────────────────────── */

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo() {
    return NvrPluginInfo{
        PLUGIN_NAME,
        PLUGIN_DESC,
        /* version */ 1, 0, 0
    };
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    if (!ctx) return -1;

    /*
     * Resolve config path relative to the executable.
     * In practice the host would pass this, but we default to a
     * well-known location next to the plugin DLL.
     */
    const char* config_path = "broadcaster_bridge_config.json";

    int result = nevr::broadcaster_bridge::Initialize(ctx->base_addr, config_path);
    if (result != 0) {
        std::fprintf(stderr, "[broadcaster_bridge] Initialize failed: %d\n", result);
    }
    return result;
}

NEVR_PLUGIN_API void NvrPluginOnFrame(const NvrGameContext*) {
    nevr::broadcaster_bridge::OnFrame();
}

NEVR_PLUGIN_API void NvrPluginShutdown() {
    nevr::broadcaster_bridge::Shutdown();
}

/* ── DllMain for standalone injection ─────────────────────────────── */

#ifdef _WIN32
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)hModule;
    (void)reserved;

    /*
     * Do NOT call Initialize/Shutdown here.
     * The plugin loader calls LoadLibraryA (which triggers DllMain) and then
     * calls NvrPluginInit separately.  Auto-init from DllMain caused
     * double-initialization, crashes, and wrong timing.
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
