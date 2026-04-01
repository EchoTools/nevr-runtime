#include "device_auth.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#endif

#include "nevr_curl.h"
#include "auth_token.h"
#include "auth_token_refresh.h"

void DeviceAuth::Configure(const std::string& url, const std::string& httpKey, const std::string& serverKey) {
    m_url = url;
    m_httpKey = httpKey;
    m_serverKey = serverKey;
    m_configured = true;
    fprintf(stderr, "[NEVR.AUTH] Configured: url=%s\n", url.c_str());
}

bool DeviceAuth::IsAuthenticated() const {
    return !m_token.empty() && static_cast<uint64_t>(time(nullptr)) < m_tokenExpiry;
}

bool DeviceAuth::TryLoadCachedToken() {
    auto auth = LoadCachedAuthToken();
    if (auth.token.empty()) return false;

    // Valid access token — use it directly
    if (auth.HasValidToken()) {
        m_token = auth.token;
        m_tokenExpiry = auth.token_expiry;
        m_refreshToken = auth.refresh_token;
        m_refreshTokenExpiry = auth.refresh_token_expiry;
        m_userId = auth.user_id;
        m_username = auth.username;

        uint64_t remaining = (auth.token_expiry - static_cast<uint64_t>(time(nullptr))) / 60;
        fprintf(stderr, "[NEVR.AUTH] Loaded cached token (expires in %llum)\n",
            (unsigned long long)remaining);
        return true;
    }

    // Access token expired — try refresh
    if (auth.HasValidRefreshToken() && m_configured) {
        fprintf(stderr, "[NEVR.AUTH] Access token expired, attempting refresh...\n");
        if (RefreshAuthToken(auth, m_url, m_serverKey)) {
            m_token = auth.token;
            m_tokenExpiry = auth.token_expiry;
            m_refreshToken = auth.refresh_token;
            m_refreshTokenExpiry = auth.refresh_token_expiry;
            m_userId = auth.user_id;
            m_username = auth.username;
            return true;
        }
        fprintf(stderr, "[NEVR.AUTH] Token refresh failed -- will re-authenticate\n");
    } else if (!auth.refresh_token.empty()) {
        fprintf(stderr, "[NEVR.AUTH] Both tokens expired -- will re-authenticate\n");
    } else {
        fprintf(stderr, "[NEVR.AUTH] Cached token expired, no refresh token -- will re-authenticate\n");
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
        fprintf(stderr, "[NEVR.AUTH] Failed to write .credentials.json\n");
        return false;
    }

    fprintf(stderr, "[NEVR.AUTH] Token saved to .credentials.json\n");
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
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[NEVR.AUTH] POST %s failed: %s\n", url.c_str(), curl_easy_strerror(res));
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
        // Any unrecognized status (including "pending") keeps polling
        return "pending";
    } catch (...) {
        return "error";
    }
}

void DeviceAuth::DisplayLinkingCode(const std::string& code) {
    fprintf(stderr, "[NEVR.AUTH]\n");
    fprintf(stderr, "[NEVR.AUTH] +------------------------------------------+\n");
    fprintf(stderr, "[NEVR.AUTH] |                                          |\n");
    fprintf(stderr, "[NEVR.AUTH] |   Link your account at:                  |\n");
    fprintf(stderr, "[NEVR.AUTH] |   https://echovrce.com/login/device      |\n");
    fprintf(stderr, "[NEVR.AUTH] |                                          |\n");
    fprintf(stderr, "[NEVR.AUTH] |   Your code:   %-8s                 |\n", code.c_str());
    fprintf(stderr, "[NEVR.AUTH] |                                          |\n");
    fprintf(stderr, "[NEVR.AUTH] |   Code expires in 5 minutes.             |\n");
    fprintf(stderr, "[NEVR.AUTH] +------------------------------------------+\n");
    fprintf(stderr, "[NEVR.AUTH]\n");
}

bool DeviceAuth::RunDeviceAuthFlow() {
    if (!m_configured) {
        fprintf(stderr, "[NEVR.AUTH] Cannot run device auth -- not configured\n");
        return false;
    }

    // Step 1: Request a device code
    std::string code = RequestDeviceCode();
    if (code.empty()) {
        fprintf(stderr, "[NEVR.AUTH] Failed to request device auth code\n");
        return false;
    }

    // Step 2: Display code and open browser
    DisplayLinkingCode(code);

#ifdef _WIN32
    ShellExecuteA(NULL, "open", "https://echovrce.com/login/device", NULL, NULL, SW_SHOWNORMAL);
#endif

    // Step 3: Poll every 3 seconds for up to 5 minutes
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
            fprintf(stderr, "[NEVR.AUTH] Device authorized! Signed in successfully.\n");
            SaveToken();
            return true;
        }
        if (status == "expired") {
            fprintf(stderr, "[NEVR.AUTH] Device code expired. Please restart to try again.\n");
            return false;
        }
        if (status == "error") {
            fprintf(stderr, "[NEVR.AUTH] Error polling device code\n");
            return false;
        }
        // status == "pending" -- keep polling
        if (i % 10 == 9) {
            fprintf(stderr, "[NEVR.AUTH] Still waiting for authorization... (%ds remaining)\n",
                (maxPolls - i) * 3);
        }
    }

    fprintf(stderr, "[NEVR.AUTH] Device auth timed out after 5 minutes.\n");
    return false;
}
