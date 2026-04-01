#pragma once

// NEVR REST API client for pnsrad social feature bridge.
// Uses libcurl (already a vcpkg dep linked to gamepatches).
// Talks to the echovrce Nakama backend via standard REST API.

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

struct NevrFriend {
    std::string userId;
    std::string username;
    std::string displayName;
    bool online = false;
    int state = 0;  // 0=friend, 1=invite_sent, 2=invite_received, 3=blocked
};

class NevrClient {
public:
    NevrClient() = default;
    ~NevrClient() = default;

    // Configure from game's config.json values (url + httpKey required)
    void Configure(const std::string& url, const std::string& httpKey,
                   const std::string& serverKey = "",
                   const std::string& username = "",
                   const std::string& password = "");

    // Authenticate via password -- used by servers only.
    // Clients use token-auth plugin for authentication.
    bool Authenticate();

    // Get current token, refreshing if expired. Returns empty string on failure.
    std::string GetToken();

    // Check if we have a valid (non-expired) session
    bool IsAuthenticated() const;

    // Friends API
    bool ListFriends(int state, std::vector<NevrFriend>& outFriends);
    bool AddFriend(const std::string& userId);
    bool AddFriendByUsername(const std::string& username);
    bool DeleteFriend(const std::string& userId);
    bool BlockFriend(const std::string& userId);

    // Party API (via Nakama storage-backed RPCs)
    struct PartyMember { std::string userId; std::string username; };
    bool CreateParty(std::string& outPartyId, int maxSize = 4, bool open = true);
    bool JoinParty(const std::string& partyId);
    bool LeaveParty(const std::string& partyId);
    bool KickMember(const std::string& partyId, const std::string& userId);
    bool PromoteMember(const std::string& partyId, const std::string& userId);
    bool ListPartyMembers(const std::string& partyId, std::vector<PartyMember>& outMembers);
    std::string GetCurrentPartyId() const { return m_currentPartyId; }

    // Set token directly (e.g., loaded from auth.json by token-auth plugin)
    void SetToken(const std::string& token, uint64_t expiry);

private:
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
    std::string m_currentPartyId;
    uint64_t m_tokenExpiry = 0;
    bool m_configured = false;
};
