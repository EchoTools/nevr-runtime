#include "device_auth.h"

#include <curl/curl.h>
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

// Simple JSON string extraction (same pattern as social_bridge.cpp)
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

// Extract a numeric value from JSON (unquoted)
static uint64_t ExtractJsonUint64(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return 0;

    // Skip whitespace
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        pos++;

    return std::strtoull(json.c_str() + pos, nullptr, 10);
}

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

static std::string ReadFileContents(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0 || len > 1024 * 1024) {
        fclose(f);
        return {};
    }
    fseek(f, 0, SEEK_SET);

    std::string contents(static_cast<size_t>(len), '\0');
    size_t read = fread(&contents[0], 1, static_cast<size_t>(len), f);
    fclose(f);
    contents.resize(read);
    return contents;
}

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
    std::string json = ReadFileContents("_local/auth.json");
    if (json.empty()) return false;

    std::string token = ExtractJsonString(json, "token");
    uint64_t expiry = ExtractJsonUint64(json, "expiry");

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
}

bool DeviceAuth::SaveToken() {
    if (m_token.empty()) return false;

#ifdef _WIN32
    _mkdir("_local");
#else
    // On non-Windows (shouldn't happen in production, but for completeness)
    system("mkdir -p _local");
#endif

    std::ofstream out("_local/auth.json", std::ios::trunc);
    if (!out.is_open()) {
        fprintf(stderr, "[NEVR.AUTH] Failed to write _local/auth.json\n");
        return false;
    }

    out << "{\n"
        << "  \"token\": \"" << m_token << "\",\n"
        << "  \"expiry\": " << m_tokenExpiry << "\n"
        << "}\n";
    out.close();

    fprintf(stderr, "[NEVR.AUTH] Token saved to _local/auth.json\n");
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
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

    // Extract code from: {"code":"XXXX-XXXX","expires_in":300}
    std::string code = ExtractJsonString(response, "code");
    return code;
}

std::string DeviceAuth::PollDeviceCode(const std::string& code) {
    std::string url = m_url + "/v2/rpc/device/auth/poll?http_key=" + m_httpKey;
    std::string body = "{\"code\":\"" + code + "\"}";
    std::string response = HttpPostPublic(url, body);
    if (response.empty()) return "error";

    if (response.find("\"status\":\"verified\"") != std::string::npos) {
        std::string token = ExtractJsonString(response, "token");
        if (!token.empty()) {
            m_token = token;
            m_tokenExpiry = static_cast<uint64_t>(time(nullptr)) + (50 * 60);
            return "verified";
        }
    }
    if (response.find("\"status\":\"expired\"") != std::string::npos) return "expired";
    if (response.find("\"status\":\"pending\"") != std::string::npos) return "pending";
    return "error";
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
