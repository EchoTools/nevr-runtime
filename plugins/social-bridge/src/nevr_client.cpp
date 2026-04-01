#include "nevr_client.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <ctime>
#include <cstring>
#include <cstdio>

#include "nevr_curl.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

void NevrClient::Configure(const std::string& url, const std::string& httpKey,
                               const std::string& serverKey, const std::string& username,
                               const std::string& password) {
    m_url = url;
    m_httpKey = httpKey;
    m_serverKey = serverKey;
    m_username = username;
    m_password = password;
    m_configured = true;
    fprintf(stderr, "[NEVR.SOCIAL] API configured: url=%s, user=%s\n", url.c_str(), username.c_str());
}

bool NevrClient::Authenticate() {
    if (!m_configured) {
        fprintf(stderr, "[NEVR.SOCIAL] API not configured -- call Configure() first\n");
        return false;
    }
    if (m_username.empty()) {
        fprintf(stderr, "[NEVR.SOCIAL] No credentials for password auth (use token-auth plugin)\n");
        return false;
    }

    // POST {url}/v2/rpc/account/authenticate/password?unwrap&http_key={httpKey}
    std::string authUrl = m_url + "/v2/rpc/account/authenticate/password?unwrap&http_key=" + m_httpKey;
    std::string body = "{\"username\":\"" + m_username + "\",\"password\":\"" + m_password + "\",\"create\":true}";

    CURL* curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[NEVR.SOCIAL] Failed to init curl\n");
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nevr::CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    #ifdef NEVR_INSECURE_SKIP_TLS_VERIFY
#ifdef NEVR_INSECURE_SKIP_TLS_VERIFY
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
#endif
#endif

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[NEVR.SOCIAL] Auth request failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    if (httpCode != 200) {
        fprintf(stderr, "[NEVR.SOCIAL] Auth failed (HTTP %ld): %s\n",
            httpCode, response.substr(0, 200).c_str());
        return false;
    }

    // Extract token from JSON response: {"token":"eyJ...","refresh_token":"..."}
    try {
        auto j = nlohmann::json::parse(response);
        m_token = j["token"].get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        fprintf(stderr, "[NEVR.SOCIAL] Failed to parse auth response: %s\n", e.what());
        return false;
    }

    if (m_token.empty()) {
        fprintf(stderr, "[NEVR.SOCIAL] No token in auth response\n");
        return false;
    }

    // Token expires in 60 min, refresh at 50 min
    m_tokenExpiry = static_cast<uint64_t>(time(nullptr)) + (50 * 60);

    fprintf(stderr, "[NEVR.SOCIAL] Authenticated successfully (token expires in 50min)\n");
    return true;
}

std::string NevrClient::GetToken() {
    if (!RefreshTokenIfNeeded()) return "";
    return m_token;
}

bool NevrClient::IsAuthenticated() const {
    return !m_token.empty() && static_cast<uint64_t>(time(nullptr)) < m_tokenExpiry;
}

bool NevrClient::RefreshTokenIfNeeded() {
    if (IsAuthenticated()) return true;
    fprintf(stderr, "[NEVR.SOCIAL] Token expired or missing -- re-authenticating\n");
    return Authenticate();
}

std::string NevrClient::HttpGet(const std::string& url) {
    if (!RefreshTokenIfNeeded()) return "";

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + m_token;
    headers = curl_slist_append(headers, authHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
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
        fprintf(stderr, "[NEVR.SOCIAL] GET %s failed: %s\n", url.c_str(), curl_easy_strerror(res));
        return "";
    }
    return response;
}

std::string NevrClient::HttpPost(const std::string& url, const std::string& body) {
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
        fprintf(stderr, "[NEVR.SOCIAL] POST %s failed: %s\n", url.c_str(), curl_easy_strerror(res));
        return "";
    }
    return response;
}

std::string NevrClient::HttpDelete(const std::string& url, const std::string& body) {
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
        fprintf(stderr, "[NEVR.SOCIAL] DELETE %s failed: %s\n", url.c_str(), curl_easy_strerror(res));
        return "";
    }
    return response;
}

bool NevrClient::ListFriends(int state, std::vector<NevrFriend>& outFriends) {
    std::string url = m_url + "/v2/friend?limit=100";
    if (state >= 0) {
        url += "&state=" + std::to_string(state);
    }

    std::string response = HttpGet(url);
    if (response.empty()) return false;

    // Parse friend list from JSON response
    // Format: {"friends":[{"user":{"id":"...","username":"...","display_name":"...","online":true},"state":0},...]}
    try {
        auto j = nlohmann::json::parse(response);
        for (const auto& entry : j.value("friends", nlohmann::json::array())) {
            NevrFriend f;
            if (entry.contains("user")) {
                const auto& user = entry["user"];
                f.userId = user.value("id", "");
                f.username = user.value("username", "");
                f.displayName = user.value("display_name", "");
                f.online = user.value("online", false);
            }
            f.state = entry.value("state", 0);
            outFriends.push_back(f);
        }
    } catch (const nlohmann::json::exception& e) {
        fprintf(stderr, "[NEVR.SOCIAL] Failed to parse friends response: %s\n", e.what());
        return false;
    }

    fprintf(stderr, "[NEVR.SOCIAL] Listed %zu friends (state=%d)\n", outFriends.size(), state);
    return true;
}

bool NevrClient::AddFriend(const std::string& userId) {
    std::string url = m_url + "/v2/friend";
    std::string body = "{\"ids\":[\"" + userId + "\"]}";
    std::string response = HttpPost(url, body);
    fprintf(stderr, "[NEVR.SOCIAL] AddFriend(%s): %s\n", userId.c_str(),
        response.empty() ? "failed" : "ok");
    return !response.empty();
}

bool NevrClient::AddFriendByUsername(const std::string& username) {
    std::string url = m_url + "/v2/friend";
    std::string body = "{\"usernames\":[\"" + username + "\"]}";
    std::string response = HttpPost(url, body);
    fprintf(stderr, "[NEVR.SOCIAL] AddFriendByUsername(%s): %s\n", username.c_str(),
        response.empty() ? "failed" : "ok");
    return !response.empty();
}

bool NevrClient::DeleteFriend(const std::string& userId) {
    std::string url = m_url + "/v2/friend?ids=" + userId;
    std::string response = HttpDelete(url, "");
    fprintf(stderr, "[NEVR.SOCIAL] DeleteFriend(%s): %s\n", userId.c_str(),
        response.empty() ? "failed" : "ok");
    return true;  // DELETE returns empty body on success
}

bool NevrClient::BlockFriend(const std::string& userId) {
    std::string url = m_url + "/v2/friend/block";
    std::string body = "{\"ids\":[\"" + userId + "\"]}";
    std::string response = HttpPost(url, body);
    fprintf(stderr, "[NEVR.SOCIAL] BlockFriend(%s): %s\n", userId.c_str(),
        response.empty() ? "failed" : "ok");
    return !response.empty();
}

// ============================================================================
// Party API
// ============================================================================

bool NevrClient::CreateParty(std::string& outPartyId, int maxSize, bool open) {
    std::string url = m_url + "/v2/rpc/party/create?unwrap&http_key=" + m_httpKey;
    std::string body = "{\"max_size\":" + std::to_string(maxSize) +
                       ",\"open\":" + (open ? "true" : "false") + "}";
    std::string response = HttpPost(url, body);
    if (response.empty()) return false;

    try {
        auto j = nlohmann::json::parse(response);
        outPartyId = j["party_id"].get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        fprintf(stderr, "[NEVR.SOCIAL] Failed to parse CreateParty response: %s\n", e.what());
        return false;
    }
    m_currentPartyId = outPartyId;
    fprintf(stderr, "[NEVR.SOCIAL] CreateParty: %s\n", outPartyId.c_str());
    return true;
}

bool NevrClient::JoinParty(const std::string& partyId) {
    std::string url = m_url + "/v2/rpc/party/join?unwrap&http_key=" + m_httpKey;
    std::string body = "{\"party_id\":\"" + partyId + "\"}";
    std::string response = HttpPost(url, body);
    if (response.empty()) return false;
    m_currentPartyId = partyId;
    fprintf(stderr, "[NEVR.SOCIAL] JoinParty: %s\n", partyId.c_str());
    return true;
}

bool NevrClient::LeaveParty(const std::string& partyId) {
    std::string url = m_url + "/v2/rpc/party/leave?unwrap&http_key=" + m_httpKey;
    std::string body = "{\"party_id\":\"" + partyId + "\"}";
    std::string response = HttpPost(url, body);
    if (m_currentPartyId == partyId) m_currentPartyId.clear();
    fprintf(stderr, "[NEVR.SOCIAL] LeaveParty: %s\n", partyId.c_str());
    return true;
}

bool NevrClient::KickMember(const std::string& partyId, const std::string& userId) {
    std::string url = m_url + "/v2/rpc/party/kick?unwrap&http_key=" + m_httpKey;
    std::string body = "{\"party_id\":\"" + partyId + "\",\"target_id\":\"" + userId + "\"}";
    std::string response = HttpPost(url, body);
    fprintf(stderr, "[NEVR.SOCIAL] KickMember(%s from %s): %s\n",
        userId.c_str(), partyId.c_str(), response.empty() ? "failed" : "ok");
    return !response.empty();
}

bool NevrClient::PromoteMember(const std::string& partyId, const std::string& userId) {
    std::string url = m_url + "/v2/rpc/party/promote?unwrap&http_key=" + m_httpKey;
    std::string body = "{\"party_id\":\"" + partyId + "\",\"target_id\":\"" + userId + "\"}";
    std::string response = HttpPost(url, body);
    fprintf(stderr, "[NEVR.SOCIAL] PromoteMember(%s in %s): %s\n",
        userId.c_str(), partyId.c_str(), response.empty() ? "failed" : "ok");
    return !response.empty();
}

bool NevrClient::ListPartyMembers(const std::string& partyId, std::vector<PartyMember>& outMembers) {
    std::string url = m_url + "/v2/rpc/party/members?unwrap&http_key=" + m_httpKey;
    std::string body = "{\"party_id\":\"" + partyId + "\"}";
    std::string response = HttpPost(url, body);
    if (response.empty()) return false;

    // Parse members array from JSON
    try {
        auto j = nlohmann::json::parse(response);
        for (const auto& entry : j.value("members", nlohmann::json::array())) {
            PartyMember m;
            m.userId = entry.value("user_id", "");
            m.username = entry.value("username", "");
            outMembers.push_back(m);
        }
    } catch (const nlohmann::json::exception& e) {
        fprintf(stderr, "[NEVR.SOCIAL] Failed to parse ListPartyMembers response: %s\n", e.what());
        return false;
    }
    fprintf(stderr, "[NEVR.SOCIAL] ListPartyMembers(%s): %zu members\n", partyId.c_str(), outMembers.size());
    return true;
}

// ============================================================================
// Device Code Authentication
// ============================================================================

void NevrClient::SetToken(const std::string& token, uint64_t expiry) {
    m_token = token;
    m_tokenExpiry = expiry;
}
