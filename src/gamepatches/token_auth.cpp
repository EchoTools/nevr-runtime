/* SYNTHESIS -- custom tool code, not from binary */

#include "token_auth.h"
#include "config.h"
#include "common/logging.h"

#include "common/auth_token.h"
#include "auth_token_refresh.h"
#include "nevr_curl.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <ctime>
#include <string>

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
    if (auth.token.empty()) return false;

    if (auth.HasValidToken()) {
        m_token = auth.token;
        m_tokenExpiry = auth.token_expiry;
        m_refreshToken = auth.refresh_token;
        m_refreshTokenExpiry = auth.refresh_token_expiry;
        m_userId = auth.user_id;
        m_username = auth.username;

        uint64_t remaining = (auth.token_expiry - static_cast<uint64_t>(time(nullptr))) / 60;
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Loaded cached token (expires in %llum)",
            (unsigned long long)remaining);
        return true;
    }

    if (auth.HasValidRefreshToken() && m_configured) {
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Access token expired, attempting refresh...");
        if (RefreshAuthToken(auth, m_url, m_serverKey)) {
            m_token = auth.token;
            m_tokenExpiry = auth.token_expiry;
            m_refreshToken = auth.refresh_token;
            m_refreshTokenExpiry = auth.refresh_token_expiry;
            m_userId = auth.user_id;
            m_username = auth.username;
            return true;
        }
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Token refresh failed -- will re-authenticate");
    } else if (!auth.refresh_token.empty()) {
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Both tokens expired -- will re-authenticate");
    } else {
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Cached token expired, no refresh token -- will re-authenticate");
    }

    return false;
}

bool DeviceAuth::SaveToken() {
    if (m_token.empty()) return false;

    CachedAuthToken auth;
    auth.token = m_token;
    auth.token_expiry = m_tokenExpiry;
    auth.refresh_token = m_refreshToken;
    auth.refresh_token_expiry = m_refreshTokenExpiry;
    auth.user_id = m_userId;
    auth.username = m_username;

    if (!SaveAuthToken(auth)) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Failed to write .credentials.json");
        return false;
    }

    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Token saved to .credentials.json");
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
    std::string url = m_url + "/v2/rpc/device/auth/request?http_key=" + m_httpKey;
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
    std::string url = m_url + "/v2/rpc/device/auth/poll?http_key=" + m_httpKey;
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
                m_tokenExpiry = static_cast<uint64_t>(time(nullptr)) + 3600;
                m_refreshToken = j.value("refresh_token", "");
                m_refreshTokenExpiry = static_cast<uint64_t>(time(nullptr)) + (30 * 24 * 3600);
                m_userId = j.value("user_id", "");
                m_username = j.value("username", "");
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
            Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Still waiting for authorization... (%ds remaining)",
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

// Read config from the already-loaded early config (g_earlyConfigPtr).
// LoadEarlyConfig() in config.cpp handles the search-path logic.
struct AuthConfig {
    std::string url;
    std::string httpKey;
    std::string serverKey;
};

static AuthConfig LoadAuthConfig() {
    AuthConfig cfg;

    if (g_earlyConfigPtr) {
        CHAR* url = EchoVR::JsonValueAsString(g_earlyConfigPtr, (CHAR*)"nevr_http_uri", NULL, false);
        CHAR* key = EchoVR::JsonValueAsString(g_earlyConfigPtr, (CHAR*)"nevr_http_key", NULL, false);
        CHAR* skey = EchoVR::JsonValueAsString(g_earlyConfigPtr, (CHAR*)"nevr_server_key", NULL, false);
        if (url) cfg.url = url;
        if (key) cfg.httpKey = key;
        if (skey) cfg.serverKey = skey;
    }

    return cfg;
}

} // anonymous namespace

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
        return;
    }

    // No cached credentials — run device auth now, before game connections start.
    s_authAttempted = true;
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] No cached credentials — starting device code auth...");
    if (s_auth->RunDeviceAuthFlow()) {
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Authenticated via Discord");
    } else {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Authentication failed -- social features may be limited");
    }
}

void TokenAuth::Shutdown() {
    delete s_auth;
    s_auth = nullptr;
    s_authAttempted = false;
    Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Shutdown complete");
}
