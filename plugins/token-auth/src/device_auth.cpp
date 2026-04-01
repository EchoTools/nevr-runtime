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

void DeviceAuth::Configure(const std::string& url, const std::string& httpKey) {
    m_url = url;
    m_httpKey = httpKey;
    m_configured = true;
    fprintf(stderr, "[NEVR.AUTH] Configured: url=%s\n", url.c_str());
}

bool DeviceAuth::IsAuthenticated() const {
    return !m_token.empty() && static_cast<uint64_t>(time(nullptr)) < m_tokenExpiry;
}

bool DeviceAuth::TryLoadCachedToken() {
    // Search _local/auth.json with parent-directory fallback
    const char* paths[] = {"_local/auth.json", "..\\_local\\auth.json",
                           "..\\..\\_local\\auth.json", "../_local/auth.json",
                           "../../_local/auth.json"};
    std::string contents;
    for (const auto* p : paths) {
        std::ifstream f(p, std::ios::binary);
        if (f.is_open()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            contents = ss.str();
            break;
        }
    }
    if (contents.empty()) return false;

    try {
        auto j = nlohmann::json::parse(contents);
        std::string token = j.value("token", "");
        uint64_t expiry = j.value("expiry", uint64_t(0));

        if (token.empty() || expiry == 0) {
            fprintf(stderr, "[NEVR.AUTH] auth.json malformed -- will re-authenticate\n");
            return false;
        }

        uint64_t now = static_cast<uint64_t>(time(nullptr));
        if (expiry <= now + 60) {
            fprintf(stderr, "[NEVR.AUTH] Cached token expired -- will re-authenticate\n");
            return false;
        }

        m_token = token;
        m_tokenExpiry = expiry;
        fprintf(stderr, "[NEVR.AUTH] Loaded cached token (expires in %llum)\n",
            (unsigned long long)((expiry - now) / 60));
        return true;
    } catch (const nlohmann::json::parse_error&) {
        fprintf(stderr, "[NEVR.AUTH] auth.json parse error -- will re-authenticate\n");
        return false;
    }
}

bool DeviceAuth::SaveToken() {
    if (m_token.empty()) return false;

    // Find existing _local/ dir with parent-directory fallback
    const char* dirs[] = {"_local", "..\\_local", "..\\..\\_local",
                          "../_local", "../../_local"};
    std::string target;
    for (const auto* d : dirs) {
        std::string probe = std::string(d) + "/config.json";
        if (std::ifstream(probe).is_open()) {
            target = std::string(d) + "/auth.json";
            break;
        }
    }
    if (target.empty()) {
#ifdef _WIN32
        _mkdir("_local");
#endif
        target = "_local/auth.json";
    }

    std::ofstream out(target, std::ios::trunc);
    if (!out.is_open()) {
        fprintf(stderr, "[NEVR.AUTH] Failed to write %s\n", target.c_str());
        return false;
    }

    nlohmann::json j;
    j["token"] = m_token;
    j["expiry"] = m_tokenExpiry;
    out << j.dump(2) << "\n";
    out.close();

    fprintf(stderr, "[NEVR.AUTH] Token saved to %s\n", target.c_str());
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
                m_tokenExpiry = j.value("expiry", static_cast<uint64_t>(time(nullptr)) + 3600);
                return "verified";
            }
        }
        if (status == "expired") return "expired";
        if (status == "pending") return "pending";
        return "error";
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
