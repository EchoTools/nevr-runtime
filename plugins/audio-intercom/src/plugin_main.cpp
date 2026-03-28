/* SYNTHESIS -- custom tool code, not from binary */

#include "audio_intercom.h"
#include "nevr_plugin_interface.h"

/* ========================================================================
 * nEVR plugin interface exports
 * ======================================================================== */

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo() {
    NvrPluginInfo info{};
    info.name          = "audio_intercom";
    info.description   = "UDP-fed Opus audio intercom injector for EchoVR VoIP";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    if (!ctx) return -1;

    /* Resolve config path relative to the plugin DLL or use default */
    const char* config_path = "audio_intercom_config.json";

    return nevr::audio_intercom::Initialize(ctx->base_addr, config_path);
}

NEVR_PLUGIN_API void NvrPluginShutdown() {
    nevr::audio_intercom::Shutdown();
}

/* ========================================================================
 * DllMain for standalone loading
 * ======================================================================== */
#if defined(_WIN32)
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)hModule;
    (void)lpReserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            break;
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
#endif
