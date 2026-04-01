/* SYNTHESIS -- custom tool code, not from binary */

#include "nevr_plugin_interface.h"
#include "nevr_common.h"
#include "device_auth.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <string>

static DeviceAuth* s_auth = nullptr;
static bool s_authAttempted = false;

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "token_auth";
    info.description = "Device code authentication with credential caching";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    // Servers use password auth, not device code
    if (ctx->flags & NEVR_HOST_IS_SERVER) {
        fprintf(stderr, "[NEVR.AUTH] Running in server mode -- token auth disabled\n");
        return 0;
    }

    s_auth = new DeviceAuth();

    // Load config from _local/config.json
    std::string configStr = nevr::LoadConfigFile("_local/config.json");
    if (configStr.empty()) {
        fprintf(stderr, "[NEVR.AUTH] Could not read _local/config.json\n");
        return 0;
    }

    std::string nevrUrl, nevrHttpKey, nevrServerKey;
    try {
        auto cfg = nlohmann::json::parse(configStr);
        nevrUrl = cfg.value("nevr_url", "");
        nevrHttpKey = cfg.value("nevr_http_key", "");
        nevrServerKey = cfg.value("nevr_server_key", "");
    } catch (const nlohmann::json::parse_error& e) {
        fprintf(stderr, "[NEVR.AUTH] Failed to parse config.json: %s\n", e.what());
        return 0;
    }

    if (nevrUrl.empty() || nevrHttpKey.empty()) {
        fprintf(stderr, "[NEVR.AUTH] Missing nevr_url or nevr_http_key in config.json\n");
        return 0;
    }

    s_auth->Configure(nevrUrl, nevrHttpKey, nevrServerKey);

    // Try cached token first
    if (s_auth->TryLoadCachedToken()) {
        fprintf(stderr, "[NEVR.AUTH] Using cached credentials -- no login needed\n");
        s_authAttempted = true;
    }

    return 0;
}

NEVR_PLUGIN_API void NvrPluginOnGameStateChange(const NvrGameContext* ctx, uint32_t old_state, uint32_t new_state) {
    // Run device auth when entering Lobby (state 5) if not already authenticated
    if (!s_auth || s_authAttempted) return;
    if (ctx->flags & NEVR_HOST_IS_SERVER) return;
    if (new_state != 5) return;  // NetGameState::Lobby == 5

    s_authAttempted = true;

    fprintf(stderr, "[NEVR.AUTH] Starting device code authentication (Discord OAuth)...\n");
    if (s_auth->RunDeviceAuthFlow()) {
        fprintf(stderr, "[NEVR.AUTH] Authenticated via Discord\n");
    } else {
        fprintf(stderr, "[NEVR.AUTH] Authentication failed -- social features may be limited\n");
    }
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
    if (s_auth) {
        delete s_auth;
        s_auth = nullptr;
    }
    s_authAttempted = false;
    fprintf(stderr, "[NEVR.AUTH] Shutdown complete\n");
}
