/* SYNTHESIS -- custom tool code, not from binary */

#include "nevr_plugin_interface.h"
#include "nevr_common.h"
#include "device_auth.h"

#include <cstdio>
#include <cstring>
#include <string>

static DeviceAuth* s_auth = nullptr;
static bool s_authAttempted = false;

// Simple JSON string extraction for config loading
static std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};

    size_t q1 = json.find('"', pos + 1);
    if (q1 == std::string::npos) return {};

    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};

    return json.substr(q1 + 1, q2 - q1 - 1);
}

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
    std::string configJson = nevr::LoadConfigFile("_local/config.json");
    if (configJson.empty()) {
        fprintf(stderr, "[NEVR.AUTH] Could not read _local/config.json\n");
        return 0;
    }

    std::string nevrUrl = ExtractJsonString(configJson, "nevr_url");
    std::string nevrHttpKey = ExtractJsonString(configJson, "nevr_http_key");

    if (nevrUrl.empty() || nevrHttpKey.empty()) {
        fprintf(stderr, "[NEVR.AUTH] Missing nevr_url or nevr_http_key in config.json\n");
        return 0;
    }

    s_auth->Configure(nevrUrl, nevrHttpKey);

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
