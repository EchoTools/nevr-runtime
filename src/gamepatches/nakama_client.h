#pragma once

// Nakama REST API client for pnsrad social feature bridge.
// Uses libcurl (already a vcpkg dep linked to gamepatches).
// Auth flow from: ~/src/evr-early-quit-bot/nakama_authed_fetch.py

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

struct NakamaFriend {
    std::string userId;
    std::string username;
    std::string displayName;
    bool online = false;
    int state = 0;  // 0=friend, 1=invite_sent, 2=invite_received, 3=blocked
};

class NakamaClient {
public:
    NakamaClient() = default;
    ~NakamaClient() = default;

    // Configure from game's config.json values
    void Configure(const std::string& url, const std::string& httpKey,
                   const std::string& serverKey, const std::string& username,
                   const std::string& password);

    // Authenticate with Nakama — returns true on success, stores token internally.
    // POST {url}/v2/rpc/account/authenticate/password?unwrap&http_key={httpKey}
    bool Authenticate();

    // Get current token, refreshing if expired. Returns empty string on failure.
    std::string GetToken();

    // Check if we have a valid (non-expired) session
    bool IsAuthenticated() const;

    // Friends API
    bool ListFriends(int state, std::vector<NakamaFriend>& outFriends);
    bool AddFriend(const std::string& userId);
    bool AddFriendByUsername(const std::string& username);
    bool DeleteFriend(const std::string& userId);
    bool BlockFriend(const std::string& userId);

    // Device code authentication flow (GitHub-style)
    // Returns true if a token was obtained, false on timeout/failure.
    // Displays the code in the game log and opens the browser.
    bool RunDeviceAuthFlow();

    // Set token directly (e.g., loaded from auth.json)
    void SetToken(const std::string& token, uint64_t expiry);

private:
    // Device auth helpers
    std::string RequestDeviceCode();                    // Returns "XXXX-XXXX" code
    std::string PollDeviceCode(const std::string& code); // Returns "pending", "verified", or "expired"

    // HTTP helper that doesn't require auth (for device code endpoints)
    std::string HttpPostPublic(const std::string& url, const std::string& body);

    // HTTP helpers
    std::string HttpGet(const std::string& url);
    std::string HttpPost(const std::string& url, const std::string& body);
    std::string HttpDelete(const std::string& url, const std::string& body);

    bool RefreshTokenIfNeeded();

    std::string m_url;
    std::string m_httpKey;
    std::string m_serverKey;
    std::string m_username;
    std::string m_password;
    std::string m_token;
    uint64_t m_tokenExpiry = 0;  // Unix timestamp when token expires
    bool m_configured = false;
};
