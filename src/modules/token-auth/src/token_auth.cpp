/* SYNTHESIS -- custom tool code, not from binary */
/* Module version — uses NvrModuleContext instead of config.h globals */
/* Log levels follow the N47 taxonomy: phase outcomes at Info, per-step detail
 * at Debug. Merge 2f29312 once reverted nine Debug demotions here by taking a
 * stale branch copy wholesale — a revert-by-merge diffs clean against both
 * parents, so the N94 verify sensor pins those nine lines at Debug instead. */

#include "token_auth.h"
#include "extension/module_interface.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"

#include "core/auth_token.h"
#include "auth_token_refresh.h"
#include "nevr_curl.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif


namespace {

// ---------------------------------------------------------------------------
// DeviceAuth — adapted from plugins/token-auth/src/device_auth.{h,cpp}
// ---------------------------------------------------------------------------

class DeviceAuth {
public:
    void Configure(const std::string& url, const std::string& httpKey, const std::string& serverKey);
    bool TryLoadCachedToken();
    bool RunDeviceAuthFlow();
    bool SaveToken();
    bool IsAuthenticated() const;
    std::string GetTokenValue() const { return m_token; }
    uint64_t GetDiscordIdValue() const { return m_discordId; }
    std::string GetUsernameValue() const { return m_username; }
    void UpdateFromRefresh(const CachedAuthToken& auth);

private:
    std::string RequestDeviceCode();
    std::string PollDeviceCode(const std::string& code);
    std::string HttpPostPublic(const std::string& url, const std::string& body);
    void DisplayLinkingCode(const std::string& code);

    std::string m_url;
    std::string m_httpKey;
    std::string m_serverKey;
    std::string m_token;
    uint64_t m_tokenExpiry = 0;
    std::string m_refreshToken;
    uint64_t m_refreshTokenExpiry = 0;
    std::string m_userId;
    std::string m_username;
    uint64_t m_discordId = 0;
    bool m_configured = false;
};

void DeviceAuth::Configure(const std::string& url, const std::string& httpKey, const std::string& serverKey) {
    m_url = url;
    m_httpKey = httpKey;
    m_serverKey = serverKey;
    m_configured = true;
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Configured: url=%s", url.c_str());
}

bool DeviceAuth::IsAuthenticated() const {
    return !m_token.empty() && static_cast<uint64_t>(time(nullptr)) < m_tokenExpiry;
}

bool DeviceAuth::TryLoadCachedToken() {
    auto auth = LoadCachedAuthToken();
    // The access token is deliberately NOT persisted to disk — only the
    // refresh token is saved.  Check for either token before bailing so
    // the refresh path below is reachable when only the refresh token exists.
    if (auth.token.empty() && auth.refresh_token.empty()) return false;

    if (auth.HasValidToken()) {
        m_token = auth.token;
        m_tokenExpiry = auth.token_expiry;
        m_refreshToken = auth.refresh_token;
        m_refreshTokenExpiry = auth.refresh_token_expiry;
        m_userId = auth.user_id;
        m_username = auth.username;
        m_discordId = auth.GetDiscordId();

        uint64_t remaining = (auth.token_expiry - static_cast<uint64_t>(time(nullptr))) / 60;
        Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Loaded cached token (expires in %llum)",
            (unsigned long long)remaining);
        return true;
    }

    if (auth.HasValidRefreshToken() && m_configured) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Access token expired, attempting refresh...");
        if (RefreshAuthToken(auth, m_url, m_httpKey)) {
            m_token = auth.token;
            m_tokenExpiry = auth.token_expiry;
            m_refreshToken = auth.refresh_token;
            m_refreshTokenExpiry = auth.refresh_token_expiry;
            m_userId = auth.user_id;
            m_username = auth.username;
            m_discordId = auth.GetDiscordId();
            return true;
        }
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Token refresh failed -- will re-authenticate");
    } else if (!auth.refresh_token.empty()) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Both tokens expired -- will re-authenticate");
    } else {
        Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Cached token expired, no refresh token -- will re-authenticate");
    }

    return false;
}

void DeviceAuth::UpdateFromRefresh(const CachedAuthToken& auth) {
    m_token = auth.token;
    m_tokenExpiry = auth.token_expiry;
    if (!auth.refresh_token.empty()) {
        m_refreshToken = auth.refresh_token;
        m_refreshTokenExpiry = auth.refresh_token_expiry;
    }
    if (!auth.user_id.empty()) m_userId = auth.user_id;
    if (!auth.username.empty()) m_username = auth.username;
    m_discordId = auth.GetDiscordId();
}

bool DeviceAuth::SaveToken() {
    if (m_refreshToken.empty()) return false;

    CachedAuthToken auth;
    // Access token deliberately NOT set — SaveAuthToken only persists
    // refresh_token + user_id + username.
    auth.refresh_token = m_refreshToken;
    auth.refresh_token_expiry = m_refreshTokenExpiry;
    auth.user_id = m_userId;
    auth.username = m_username;

    if (!SaveAuthToken(auth)) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Failed to write .credentials.json");
        return false;
    }

    Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Refresh token saved to .credentials.json");
    return true;
}

std::string DeviceAuth::HttpPostPublic(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nevr::CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
#ifdef NEVR_INSECURE_SKIP_TLS_VERIFY
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
#endif

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] POST %s failed: %s",
            url.c_str(), curl_easy_strerror(res));
        return "";
    }
    return response;
}

std::string DeviceAuth::RequestDeviceCode() {
    // STRANDED FIX, recovered 2026-07-27. Commit 7a03d8b ("fix: Nakama RPC unwrap
    // and LoadLibraryA fallbacks for hook install", 2026-04-10) added `&unwrap` to
    // both device-auth endpoints in src/runtime/token_auth.cpp. This module was
    // extracted the SAME DAY and the fix never crossed. That commit fixed two
    // things; the LoadLibraryA half reached platform-compat, this half did not.
    //
    // Without &unwrap, Nakama wraps an RPC response as {"payload":"<json string>"},
    // so the parse below looks for "token"/"status" at the top level and finds
    // nothing — device auth silently never completes.
    std::string url = m_url + "/v2/rpc/device/auth/request?http_key=" + m_httpKey + "&unwrap";
    std::string response = HttpPostPublic(url, "{}");
    if (response.empty()) return "";

    try {
        auto j = nlohmann::json::parse(response);
        return j.value("code", "");
    } catch (...) {
        return "";
    }
}

std::string DeviceAuth::PollDeviceCode(const std::string& code) {
    std::string url = m_url + "/v2/rpc/device/auth/poll?http_key=" + m_httpKey + "&unwrap";
    nlohmann::json reqBody;
    reqBody["code"] = code;
    std::string response = HttpPostPublic(url, reqBody.dump());
    if (response.empty()) return "error";

    try {
        auto j = nlohmann::json::parse(response);
        std::string status = j.value("status", "");

        if (status == "verified") {
            std::string token = j.value("token", "");
            if (!token.empty()) {
                m_token = token;
                // Respect the token's OWN expiry (owner decision 2026-07-27).
                // This was `time(nullptr) + 60` — the client refreshed on its own
                // hardcoded schedule instead of the server's, discarding a token
                // that might still have been valid for an hour. The JWT `exp`
                // claim is the authority; `expires_in` from the response is the
                // first fallback; a conservative constant only if neither is
                // present.
                {
                    CachedAuthToken probe;
                    probe.token = m_token;
                    const uint64_t jwtExp = probe.GetJwtExpiry();
                    const uint64_t now = static_cast<uint64_t>(time(nullptr));
                    if (jwtExp > now) {
                        m_tokenExpiry = jwtExp;
                        Log(EchoVR::LogLevel::Info,
                            "[NEVR.AUTH] token expiry from JWT exp: %llu (%llus from now)",
                            static_cast<unsigned long long>(jwtExp),
                            static_cast<unsigned long long>(jwtExp - now));
                    } else if (j.contains("expires_in") && j["expires_in"].is_number_unsigned()) {
                        m_tokenExpiry = now + j["expires_in"].get<uint64_t>();
                        Log(EchoVR::LogLevel::Info,
                            "[NEVR.AUTH] token expiry from expires_in: %llus",
                            static_cast<unsigned long long>(j["expires_in"].get<uint64_t>()));
                    } else {
                        m_tokenExpiry = now + kFallbackAccessTokenLifetimeSec;
                        Log(EchoVR::LogLevel::Warning,
                            "[NEVR.AUTH] token carries no exp and no expires_in — "
                            "falling back to %llus",
                            static_cast<unsigned long long>(kFallbackAccessTokenLifetimeSec));
                    }
                }
                m_refreshToken = j.value("refresh_token", "");
                m_refreshTokenExpiry = static_cast<uint64_t>(time(nullptr)) + (30 * 24 * 3600);
                m_userId = j.value("user_id", "");
                m_username = j.value("username", "");
                // Parse discord ID from the JWT access token
                {
                    CachedAuthToken tmp;
                    tmp.token = m_token;
                    m_discordId = tmp.GetDiscordId();
                }
                return "verified";
            }
        }
        if (status == "expired") return "expired";
        return "pending";
    } catch (...) {
        return "error";
    }
}

void DeviceAuth::DisplayLinkingCode(const std::string& code) {
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH]");
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] +------------------------------------------+");
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] |                                          |");
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] |   Link your account at:                  |");
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] |   https://echovrce.com/login/device      |");
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] |                                          |");
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] |   Your code:   %-8s                 |", code.c_str());
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] |                                          |");
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] |   Code expires in 5 minutes.             |");
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] +------------------------------------------+");
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH]");
}

bool DeviceAuth::RunDeviceAuthFlow() {
    if (!m_configured) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Cannot run device auth -- not configured");
        return false;
    }

    std::string code = RequestDeviceCode();
    if (code.empty()) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Failed to request device auth code");
        return false;
    }

    DisplayLinkingCode(code);

#ifdef _WIN32
    std::string loginUrl = "https://echovrce.com/login/device?code=" + code;
    ShellExecuteA(NULL, "open", loginUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
#endif

    // Poll every 3 seconds for up to 5 minutes.
    // This runs on a background thread (see StartDeviceAuthBackground) so we
    // won't block the game's main loop.
    int maxPolls = 100;  // 100 * 3s = 300s = 5 minutes
    for (int i = 0; i < maxPolls; i++) {
#ifdef _WIN32
        Sleep(3000);
#else
        struct timespec ts = {3, 0};
        nanosleep(&ts, nullptr);
#endif

        std::string status = PollDeviceCode(code);
        if (status == "verified") {
            Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Device authorized! Signed in successfully.");
            SaveToken();
            return true;
        }
        if (status == "expired") {
            Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Device code expired. Please restart to try again.");
            return false;
        }
        if (status == "error") {
            Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Error polling device code");
            return false;
        }
        if (i % 10 == 9) {
            Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Still waiting for authorization... (%ds remaining)",
                (maxPolls - i) * 3);
        }
    }

    Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Device auth timed out after 5 minutes.");
    return false;
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static DeviceAuth* s_auth = nullptr;
static bool s_authAttempted = false;
static std::thread* s_refreshThread = nullptr;
static std::atomic<bool> s_refreshRunning{false};
static std::mutex s_tokenMutex;
// N133 S5: the host's config accessor (ctx->config_get), stored at init. Reads
// config.yaml through the same path the runtime uses instead of the game JSON
// (early_config). May be NULL if loaded by a pre-v2 host.
static const char* (*s_configGet)(const char*) = nullptr;

struct AuthConfig {
    std::string url;
    std::string httpKey;
    std::string serverKey;
};

static AuthConfig LoadAuthConfig() {
    AuthConfig cfg;

    if (s_configGet) {
        // config_get returns NULL for an absent/unmapped key — same "missing"
        // signal the old JsonValueAsString(..., NULL, false) form returned, so
        // an absent key keeps token_auth's existing behaviour (warn + disable).
        const char* url  = s_configGet("nevr_http_uri");
        const char* key  = s_configGet("nevr_http_key");
        const char* skey = s_configGet("nevr_server_key");
        if (url)  cfg.url = url;
        if (key)  cfg.httpKey = key;
        if (skey) cfg.serverKey = skey;
    }

    return cfg;
}

} // anonymous namespace

static void RefreshThreadFunc(std::string url, std::string httpKey) {
    while (s_refreshRunning) {
        // Sleep 60 seconds between checks
        for (int i = 0; i < 60 && s_refreshRunning; i++) {
#ifdef _WIN32
            Sleep(1000);
#else
            struct timespec ts = {1, 0};
            nanosleep(&ts, nullptr);
#endif
        }
        if (!s_refreshRunning) break;

        std::lock_guard<std::mutex> lk(s_tokenMutex);
        if (!s_auth) continue;

        // Check if token expires within 5 minutes (or already expired)
        auto cached = LoadCachedAuthToken();
        uint64_t now = static_cast<uint64_t>(time(nullptr));
        if (cached.token_expiry > now + 300) continue;  // Still valid for >5 min

        if (cached.token_expiry > now) {
            Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Token expires in %llus — refreshing",
                (unsigned long long)(cached.token_expiry - now));
        } else {
            Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Token expired %llus ago — refreshing",
                (unsigned long long)(now - cached.token_expiry));
        }

        if (cached.HasValidRefreshToken()) {
            if (RefreshAuthToken(cached, url, httpKey)) {
                Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Token refreshed successfully");
                // Update the in-memory DeviceAuth instance so GetToken/GetDiscordId
                // return the new token immediately (they no longer read from disk).
                s_auth->UpdateFromRefresh(cached);
            } else {
                Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Token refresh failed");
            }
        }
    }
}

std::string TokenAuth::GetToken() {
    std::lock_guard<std::mutex> lk(s_tokenMutex);
    if (!s_auth) return "";
    if (!s_auth->IsAuthenticated()) return "";
    return s_auth->GetTokenValue();
}

uint64_t TokenAuth::GetDiscordId() {
    std::lock_guard<std::mutex> lk(s_tokenMutex);
    if (!s_auth) return 0;
    return s_auth->GetDiscordIdValue();
}

// N123. The username was already parsed from the auth response and already
// persisted to the credential cache — it had simply never been exposed, so the
// login payload sent a hardcoded literal instead. Deliberately does NOT require
// IsAuthenticated(): a cached username from a previous session is still a truer
// answer than a constant, and the caller falls back on empty.
std::string TokenAuth::GetUsername() {
    std::lock_guard<std::mutex> lk(s_tokenMutex);
    if (!s_auth) return "";
    return s_auth->GetUsernameValue();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TokenAuth::Init(uintptr_t /*base_addr*/, bool is_server) {
    // Servers use password auth, not device code
    if (is_server) {
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Running in server mode -- token auth disabled");
        return;
    }

    AuthConfig cfg = LoadAuthConfig();
    if (cfg.url.empty() || cfg.httpKey.empty()) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Missing nevr_http_uri or nevr_http_key -- token auth disabled");
        return;
    }

    s_auth = new DeviceAuth();
    s_auth->Configure(cfg.url, cfg.httpKey, cfg.serverKey);

    // Try cached token first (with refresh if expired)
    if (s_auth->TryLoadCachedToken()) {
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Using cached credentials -- no login needed");
        s_authAttempted = true;
        // Fall through to start refresh thread below
    } else {
        // No cached credentials — run device auth now, before game connections start.
        s_authAttempted = true;
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] No cached credentials — starting device code auth...");
        if (s_auth->RunDeviceAuthFlow()) {
            Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Authenticated via Discord");
        } else {
            Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Authentication failed -- social features may be limited");
        }
    }

    // Start background refresh thread (both cached and fresh auth paths)
    if (s_auth->IsAuthenticated() && !cfg.httpKey.empty()) {
        s_refreshRunning = true;
        s_refreshThread = new std::thread(RefreshThreadFunc, cfg.url, cfg.httpKey);
    }
}

void TokenAuth::Shutdown() {
    s_refreshRunning = false;
    if (s_refreshThread) {
        s_refreshThread->join();
        delete s_refreshThread;
        s_refreshThread = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(s_tokenMutex);
        delete s_auth;
        s_auth = nullptr;
    }
    s_authAttempted = false;
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Shutdown complete");
}

// ---------------------------------------------------------------------------
// Module interface
// ---------------------------------------------------------------------------

// Thread-local buffer for GetToken C export
static thread_local std::string s_tokenBuf;
static thread_local std::string s_usernameBuf;  // N123, same lifetime contract as s_tokenBuf

NEVR_MODULE_API uint32_t token_auth_ApiVersion(void) {
    return NEVR_MODULE_API_VERSION;
}

NEVR_MODULE_API int token_auth_Init(const NvrModuleContext* ctx) {
    EchoVR::g_GameBaseAddress = (CHAR*)ctx->base_addr;
    EchoVR::InitializeFunctionPointers();
    s_configGet = ctx->config_get;  // N133 S5: read config.yaml, not early_config JSON

    bool is_server = (ctx->flags & NEVR_MODULE_HOST_IS_SERVER) != 0;
    TokenAuth::Init(ctx->base_addr, is_server);

    Log(EchoVR::LogLevel::Info, "[NEVR.MODULE] token_auth initialized");
    return 0;
}

NEVR_MODULE_API void token_auth_Shutdown(void) {
    TokenAuth::Shutdown();
}

// C exports for cross-module resolution (ws_bridge reads these via get_proc)
NEVR_MODULE_API const char* TokenAuth_GetToken(void) {
    s_tokenBuf = TokenAuth::GetToken();
    return s_tokenBuf.c_str();
}

NEVR_MODULE_API uint64_t TokenAuth_GetDiscordId(void) {
    return TokenAuth::GetDiscordId();
}

// N123. Returns "" when unknown — the caller decides what to do with an absent
// name. Same static-buffer shape as TokenAuth_GetToken above.
NEVR_MODULE_API const char* TokenAuth_GetUsername(void) {
    s_usernameBuf = TokenAuth::GetUsername();
    return s_usernameBuf.c_str();
}
