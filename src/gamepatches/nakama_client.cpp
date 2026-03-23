#include "nakama_client.h"
#include "common/logging.h"

#include <curl/curl.h>
#include <ctime>
#include <cstring>

// jsoncpp is not available — use simple manual JSON parsing for the small
// response payloads we handle. For production, consider adding jsoncpp.

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

void NakamaClient::Configure(const std::string& url, const std::string& httpKey,
                               const std::string& serverKey, const std::string& username,
                               const std::string& password) {
    m_url = url;
    m_httpKey = httpKey;
    m_serverKey = serverKey;
    m_username = username;
    m_password = password;
    m_configured = true;
    Log(EchoVR::LogLevel::Info, "[NEVR.NAKAMA] Configured: url=%s, user=%s", url.c_str(), username.c_str());
}

bool NakamaClient::Authenticate() {
    if (!m_configured) {
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] Not configured — call Configure() first");
        return false;
    }

    // POST {url}/v2/rpc/account/authenticate/password?unwrap&http_key={httpKey}
    std::string authUrl = m_url + "/v2/rpc/account/authenticate/password?unwrap&http_key=" + m_httpKey;
    std::string body = "{\"username\":\"" + m_username + "\",\"password\":\"" + m_password + "\",\"create\":true}";

    CURL* curl = curl_easy_init();
    if (!curl) {
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] Failed to init curl");
        return false;
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + m_serverKey;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, authUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // TODO: proper cert validation

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] Auth request failed: %s", curl_easy_strerror(res));
        return false;
    }

    if (httpCode != 200) {
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] Auth failed (HTTP %ld): %s",
            httpCode, response.substr(0, 200).c_str());
        return false;
    }

    // Extract token from JSON response: {"token":"eyJ...","refresh_token":"..."}
    // Simple extraction — find "token":"..." pattern
    size_t tokenStart = response.find("\"token\":\"");
    if (tokenStart == std::string::npos) {
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] No token in auth response");
        return false;
    }
    tokenStart += 9;  // skip "token":"
    size_t tokenEnd = response.find("\"", tokenStart);
    if (tokenEnd == std::string::npos) {
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] Malformed token in auth response");
        return false;
    }

    m_token = response.substr(tokenStart, tokenEnd - tokenStart);
    // Token expires in 60 min, refresh at 50 min
    m_tokenExpiry = static_cast<uint64_t>(time(nullptr)) + (50 * 60);

    Log(EchoVR::LogLevel::Info, "[NEVR.NAKAMA] Authenticated successfully (token expires in 50min)");
    return true;
}

std::string NakamaClient::GetToken() {
    if (!RefreshTokenIfNeeded()) return "";
    return m_token;
}

bool NakamaClient::IsAuthenticated() const {
    return !m_token.empty() && static_cast<uint64_t>(time(nullptr)) < m_tokenExpiry;
}

bool NakamaClient::RefreshTokenIfNeeded() {
    if (IsAuthenticated()) return true;
    Log(EchoVR::LogLevel::Info, "[NEVR.NAKAMA] Token expired or missing — re-authenticating");
    return Authenticate();
}

std::string NakamaClient::HttpGet(const std::string& url) {
    if (!RefreshTokenIfNeeded()) return "";

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + m_token;
    headers = curl_slist_append(headers, authHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] GET %s failed: %s", url.c_str(), curl_easy_strerror(res));
        return "";
    }
    return response;
}

std::string NakamaClient::HttpPost(const std::string& url, const std::string& body) {
    if (!RefreshTokenIfNeeded()) return "";

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + m_token;
    headers = curl_slist_append(headers, authHeader.c_str());
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
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] POST %s failed: %s", url.c_str(), curl_easy_strerror(res));
        return "";
    }
    return response;
}

std::string NakamaClient::HttpDelete(const std::string& url, const std::string& body) {
    if (!RefreshTokenIfNeeded()) return "";

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + m_token;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] DELETE %s failed: %s", url.c_str(), curl_easy_strerror(res));
        return "";
    }
    return response;
}

bool NakamaClient::ListFriends(int state, std::vector<NakamaFriend>& outFriends) {
    std::string url = m_url + "/v2/friend?limit=100";
    if (state >= 0) {
        url += "&state=" + std::to_string(state);
    }

    std::string response = HttpGet(url);
    if (response.empty()) return false;

    // Parse friend list from JSON response
    // Format: {"friends":[{"user":{"id":"...","username":"...","display_name":"...","online":true},"state":0},...]}
    // Simple extraction — iterate through "user" objects
    size_t pos = 0;
    while ((pos = response.find("\"user\":{", pos)) != std::string::npos) {
        NakamaFriend f;
        pos += 8;  // skip "user":{

        // Extract id
        size_t idStart = response.find("\"id\":\"", pos);
        if (idStart != std::string::npos) {
            idStart += 6;
            size_t idEnd = response.find("\"", idStart);
            if (idEnd != std::string::npos) f.userId = response.substr(idStart, idEnd - idStart);
        }

        // Extract username
        size_t unStart = response.find("\"username\":\"", pos);
        if (unStart != std::string::npos) {
            unStart += 12;
            size_t unEnd = response.find("\"", unStart);
            if (unEnd != std::string::npos) f.username = response.substr(unStart, unEnd - unStart);
        }

        // Extract display_name
        size_t dnStart = response.find("\"display_name\":\"", pos);
        if (dnStart != std::string::npos) {
            dnStart += 16;
            size_t dnEnd = response.find("\"", dnStart);
            if (dnEnd != std::string::npos) f.displayName = response.substr(dnStart, dnEnd - dnStart);
        }

        // Extract online status
        f.online = (response.find("\"online\":true", pos) != std::string::npos &&
                    response.find("\"online\":true", pos) < response.find("}", pos + 100));

        // Extract state (after the user object closes)
        size_t stateStart = response.find("\"state\":", pos);
        if (stateStart != std::string::npos && stateStart < pos + 500) {
            stateStart += 8;
            // Skip any whitespace or quotes
            while (stateStart < response.size() && (response[stateStart] == ' ' || response[stateStart] == '"'))
                stateStart++;
            f.state = std::atoi(response.c_str() + stateStart);
        }

        outFriends.push_back(f);
    }

    Log(EchoVR::LogLevel::Info, "[NEVR.NAKAMA] Listed %zu friends (state=%d)", outFriends.size(), state);
    return true;
}

bool NakamaClient::AddFriend(const std::string& userId) {
    std::string url = m_url + "/v2/friend";
    std::string body = "{\"ids\":[\"" + userId + "\"]}";
    std::string response = HttpPost(url, body);
    Log(EchoVR::LogLevel::Info, "[NEVR.NAKAMA] AddFriend(%s): %s", userId.c_str(),
        response.empty() ? "failed" : "ok");
    return !response.empty();
}

bool NakamaClient::AddFriendByUsername(const std::string& username) {
    std::string url = m_url + "/v2/friend";
    std::string body = "{\"usernames\":[\"" + username + "\"]}";
    std::string response = HttpPost(url, body);
    Log(EchoVR::LogLevel::Info, "[NEVR.NAKAMA] AddFriendByUsername(%s): %s", username.c_str(),
        response.empty() ? "failed" : "ok");
    return !response.empty();
}

bool NakamaClient::DeleteFriend(const std::string& userId) {
    std::string url = m_url + "/v2/friend?ids=" + userId;
    std::string response = HttpDelete(url, "");
    Log(EchoVR::LogLevel::Info, "[NEVR.NAKAMA] DeleteFriend(%s): %s", userId.c_str(),
        response.empty() ? "failed" : "ok");
    return true;  // DELETE returns empty body on success
}

bool NakamaClient::BlockFriend(const std::string& userId) {
    std::string url = m_url + "/v2/friend/block";
    std::string body = "{\"ids\":[\"" + userId + "\"]}";
    std::string response = HttpPost(url, body);
    Log(EchoVR::LogLevel::Info, "[NEVR.NAKAMA] BlockFriend(%s): %s", userId.c_str(),
        response.empty() ? "failed" : "ok");
    return !response.empty();
}

// ============================================================================
// Device Code Authentication
// ============================================================================

std::string NakamaClient::HttpPostPublic(const std::string& url, const std::string& body) {
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
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] POST %s failed: %s", url.c_str(), curl_easy_strerror(res));
        return "";
    }
    return response;
}

std::string NakamaClient::RequestDeviceCode() {
    std::string url = m_url + "/v2/rpc/device/auth/request?http_key=" + m_httpKey;
    std::string response = HttpPostPublic(url, "{}");
    if (response.empty()) return "";

    // Extract code from: {"code":"XXXX-XXXX","expires_in":300}
    size_t codeStart = response.find("\"code\":\"");
    if (codeStart == std::string::npos) return "";
    codeStart += 8;
    size_t codeEnd = response.find("\"", codeStart);
    if (codeEnd == std::string::npos) return "";

    return response.substr(codeStart, codeEnd - codeStart);
}

std::string NakamaClient::PollDeviceCode(const std::string& code) {
    std::string url = m_url + "/v2/rpc/device/auth/poll?http_key=" + m_httpKey;
    std::string body = "{\"code\":\"" + code + "\"}";
    std::string response = HttpPostPublic(url, body);
    if (response.empty()) return "error";

    // Check status
    if (response.find("\"status\":\"verified\"") != std::string::npos) {
        // Extract token
        size_t tokenStart = response.find("\"token\":\"");
        if (tokenStart != std::string::npos) {
            tokenStart += 9;
            size_t tokenEnd = response.find("\"", tokenStart);
            if (tokenEnd != std::string::npos) {
                m_token = response.substr(tokenStart, tokenEnd - tokenStart);
                m_tokenExpiry = static_cast<uint64_t>(time(nullptr)) + (50 * 60);
                return "verified";
            }
        }
    }
    if (response.find("\"status\":\"expired\"") != std::string::npos) return "expired";
    if (response.find("\"status\":\"pending\"") != std::string::npos) return "pending";
    return "error";
}

void NakamaClient::SetToken(const std::string& token, uint64_t expiry) {
    m_token = token;
    m_tokenExpiry = expiry;
}

bool NakamaClient::RunDeviceAuthFlow() {
    if (!m_configured) {
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] Cannot run device auth — not configured");
        return false;
    }

    // Step 1: Request a device code
    std::string code = RequestDeviceCode();
    if (code.empty()) {
        Log(EchoVR::LogLevel::Error, "[NEVR.NAKAMA] Failed to request device auth code");
        return false;
    }

    // Step 2: Display code and open browser
    Log(EchoVR::LogLevel::Info, "");
    Log(EchoVR::LogLevel::Info, "========================================");
    Log(EchoVR::LogLevel::Info, "[NEVR] Go to https://echovrce.com/login/device");
    Log(EchoVR::LogLevel::Info, "[NEVR] Enter code: %s", code.c_str());
    Log(EchoVR::LogLevel::Info, "========================================");
    Log(EchoVR::LogLevel::Info, "");

    // Try to open browser
    ShellExecuteA(NULL, "open", "https://echovrce.com/login/device", NULL, NULL, SW_SHOWNORMAL);

    // Step 3: Poll every 3 seconds for up to 5 minutes
    int maxPolls = 100;  // 100 * 3s = 300s = 5 minutes
    for (int i = 0; i < maxPolls; i++) {
        Sleep(3000);

        std::string status = PollDeviceCode(code);
        if (status == "verified") {
            Log(EchoVR::LogLevel::Info, "[NEVR] Device authorized! Signed in successfully.");
            return true;
        }
        if (status == "expired") {
            Log(EchoVR::LogLevel::Warning, "[NEVR] Device code expired. Please restart to try again.");
            return false;
        }
        if (status == "error") {
            Log(EchoVR::LogLevel::Error, "[NEVR] Error polling device code");
            return false;
        }
        // status == "pending" — keep polling
        if (i % 10 == 9) {
            Log(EchoVR::LogLevel::Info, "[NEVR] Still waiting for authorization... (%ds remaining)",
                (maxPolls - i) * 3);
        }
    }

    Log(EchoVR::LogLevel::Warning, "[NEVR] Device auth timed out after 5 minutes.");
    return false;
}
