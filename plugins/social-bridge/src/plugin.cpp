/* SYNTHESIS -- custom tool code, not from binary */

#include "nevr_plugin_interface.h"
#include "social_bridge.h"

#include <cstdio>

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "social_bridge";
    info.description = "pnsrad SNS to Nakama social feature bridge";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    // Only active on clients (servers don't need social features)
    if (ctx->flags & NEVR_HOST_IS_SERVER) {
        fprintf(stderr, "[NEVR.SOCIAL] Running in server mode -- social bridge disabled\n");
        return 0;  // Success but inactive
    }
    return SocialBridgeInit(ctx->base_addr);
}

NEVR_PLUGIN_API void NvrPluginOnGameStateChange(const NvrGameContext* ctx, uint32_t old_state, uint32_t new_state) {
    SocialBridgeOnStateChange(ctx, old_state, new_state);
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
    SocialBridgeShutdown();
}
