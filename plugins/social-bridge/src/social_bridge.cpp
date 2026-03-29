/* SYNTHESIS -- custom tool code, not from binary */

#include "social_bridge.h"
#include "social_messages.h"
#include "nevr_client.h"
#include "nevr_common.h"
#include "address_registry.h"

#include "echovr.h"
#include "symbols.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace SocialSym = EchoVR::Symbols::Social;

// ===================================================================================================
// Function pointer typedefs and storage
// ===================================================================================================

// BroadcasterListen -- registers a delegate callback for a message symbol.
// Signature from echovr_functions.h / CBroadcaster.cpp
typedef uint16_t (*BroadcasterListenFn)(void* broadcaster, int64_t msgId,
    int32_t isReliable, void* delegateProxy, int32_t prepend);
static BroadcasterListenFn s_BroadcasterListen = nullptr;

// BroadcasterReceiveLocalEvent -- dispatches a local event into the broadcaster.
// NOTE: 3rd param is const char* msgName, NOT int32_t flags
// (broadcaster_bridge uses a different signature for the Send hook)
typedef uint64_t (*BroadcasterReceiveLocalFn)(void* broadcaster, int64_t msgId,
    const char* msgName, void* msg, uint64_t msgSize);
static BroadcasterReceiveLocalFn s_BroadcasterReceiveLocal = nullptr;

// ===================================================================================================
// Module state
// ===================================================================================================

static uintptr_t s_baseAddr = 0;
static void*     s_netGame  = nullptr;
static NevrClient* s_nakamaClient = nullptr;
static bool      s_socialInitialized = false;

// ===================================================================================================
// Broadcaster pointer navigation
// ===================================================================================================

// Get the UDP broadcaster from the global game context.
// Path: base + 0x20a0478 -> game_context -> +0x8518 -> CR15NetGame
//       -> +0x28C8 -> BroadcasterData -> +0x08 -> Broadcaster*
// Offsets from: CR15NetGameLayout.h, telemetry_snapshot.h, echovr.h
static EchoVR::Broadcaster* GetBroadcasterFromGame() {
    if (!s_baseAddr) return nullptr;

    char* base = reinterpret_cast<char*>(s_baseAddr);

    // Get global game context (DAT_1420a0478)
    void** ctxPtr = reinterpret_cast<void**>(base + 0x20a0478);
    if (!ctxPtr || !*ctxPtr) return nullptr;

    // CR15NetGame = *(context + 0x8518)
    void** netGamePtr = reinterpret_cast<void**>(static_cast<char*>(*ctxPtr) + 0x8518);
    if (!netGamePtr || !*netGamePtr) return nullptr;

    // CR15NetGame + 0x28C8 = SBroadcasterData (inline)
    // SBroadcasterData + 0x08 = Broadcaster* owner
    char* netGame = static_cast<char*>(*netGamePtr);
    EchoVR::Broadcaster** ownerPtr = reinterpret_cast<EchoVR::Broadcaster**>(netGame + 0x28C8 + 0x08);
    if (!ownerPtr || !*ownerPtr) return nullptr;

    return *ownerPtr;
}

// ===================================================================================================
// Helpers
// ===================================================================================================

// Convert a 16-byte UUID to "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
static std::string UuidBytesToString(const uint8_t uuid[16]) {
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
        uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return std::string(buf);
}

// Send an SNS response back to pnsrad via BroadcasterReceiveLocalEvent.
static void SendSocialResponse(EchoVR::SymbolId hash, const char* name, void* payload, uint64_t size) {
    EchoVR::Broadcaster* broadcaster = GetBroadcasterFromGame();
    if (!broadcaster) {
        fprintf(stderr, "[NEVR.SOCIAL] Cannot send %s -- broadcaster unavailable\n", name);
        return;
    }
    s_BroadcasterReceiveLocal(broadcaster, hash, name, payload, size);
}

// Forward declarations
static void RefreshFriendsList();
static void SendPlaceholderFriendsList();

// ===================================================================================================
// Friend action handler
// ===================================================================================================

static void HandleFriendAction(EchoVR::SymbolId msgId, const SNSFriendsActionPayload* payload) {
    if (!s_nakamaClient || !s_nakamaClient->IsAuthenticated()) {
        fprintf(stderr, "[NEVR.SOCIAL] Nakama not connected -- friend action ignored\n");
        return;
    }

    std::string targetId = std::to_string(payload->target_user_id);

    if (msgId == SocialSym::FriendSendInviteRequest) {
        // Add friend by ID (or by name if target_user_id == 0)
        bool ok = (payload->target_user_id != 0)
            ? s_nakamaClient->AddFriend(targetId)
            : false;  // TODO: add_by_name needs name string from after payload

        // Send InviteSuccess or InviteFailure
        if (ok) {
            SNSFriendIdPayload resp = {};
            resp.friend_id = payload->target_user_id;
            SendSocialResponse(SocialSym::FriendInviteSuccess, "SNSFriendInviteSuccess", &resp, sizeof(resp));
        } else {
            SNSFriendNotifyPayload resp = {};
            resp.friend_id = payload->target_user_id;
            resp.status_code = static_cast<uint8_t>(EFriendInviteError::NotFound);
            SendSocialResponse(SocialSym::FriendInviteFailure, "SNSFriendInviteFailure", &resp, sizeof(resp));
        }

    } else if (msgId == SocialSym::FriendRemoveRequest) {
        s_nakamaClient->DeleteFriend(targetId);
        SNSFriendIdPayload resp = {};
        resp.friend_id = payload->target_user_id;
        SendSocialResponse(SocialSym::FriendRemoveNotify, "SNSFriendRemoveNotify", &resp, sizeof(resp));

    } else if (msgId == SocialSym::FriendActionRequest) {
        // Accept, reject, or block -- server differentiates by state.
        // For now, treat all FriendActionRequest as "accept" (most common UI action).
        // TODO: differentiate based on pnsrad's internal state for the target user.
        bool ok = s_nakamaClient->AddFriend(targetId);
        if (ok) {
            SNSFriendNotifyPayload resp = {};
            resp.friend_id = payload->target_user_id;
            SendSocialResponse(SocialSym::FriendAcceptSuccess, "SNSFriendAcceptSuccess", &resp, sizeof(resp));
        }
    }

    // Refresh friend list after any action
    RefreshFriendsList();
}

// ===================================================================================================
// Party action handler
// ===================================================================================================

static void HandlePartyAction(EchoVR::SymbolId msgId, const void* msg, uint64_t msgSize) {
    if (!s_nakamaClient || !s_nakamaClient->IsAuthenticated()) {
        fprintf(stderr, "[NEVR.SOCIAL] Not authenticated -- party action ignored\n");
        return;
    }

    std::string partyId = s_nakamaClient->GetCurrentPartyId();

    if (msgId == SocialSym::PartyKickRequest && msgSize == sizeof(SNSPartyTargetPayload)) {
        const auto* p = static_cast<const SNSPartyTargetPayload*>(msg);
        std::string targetId = UuidBytesToString(p->target_user_uuid);
        if (!partyId.empty()) {
            s_nakamaClient->KickMember(partyId, targetId);
        }
    }
    else if (msgId == SocialSym::PartyPassOwnershipRequest && msgSize == sizeof(SNSPartyTargetPayload)) {
        const auto* p = static_cast<const SNSPartyTargetPayload*>(msg);
        std::string targetId = UuidBytesToString(p->target_user_uuid);
        if (!partyId.empty()) {
            s_nakamaClient->PromoteMember(partyId, targetId);
        }
    }
    else if (msgId == SocialSym::PartyRespondToInvite && msgSize == sizeof(SNSPartyTargetPayload)) {
        const auto* p = static_cast<const SNSPartyTargetPayload*>(msg);
        if (p->param != 0 && !partyId.empty()) {
            s_nakamaClient->JoinParty(partyId);
        }
    }
    else if (msgId == SocialSym::PartyMemberUpdate && !partyId.empty()) {
        std::vector<NevrClient::PartyMember> members;
        s_nakamaClient->ListPartyMembers(partyId, members);
    }
}

// ===================================================================================================
// Social message callback
// ===================================================================================================

// Callback dispatched by the broadcaster when a social SNS message arrives from pnsrad.
static void __fastcall OnSocialMessage(void* pGame, EchoVR::SymbolId msgId, void* msg, uint64_t msgSize) {
    const char* name = "unknown";

    // Friends outgoing
    if (msgId == SocialSym::FriendSendInviteRequest) name = "FriendSendInviteRequest";
    else if (msgId == SocialSym::FriendRemoveRequest) name = "FriendRemoveRequest";
    else if (msgId == SocialSym::FriendActionRequest) name = "FriendActionRequest";
    // Party outgoing
    else if (msgId == SocialSym::PartyKickRequest) name = "PartyKickRequest";
    else if (msgId == SocialSym::PartyPassOwnershipRequest) name = "PartyPassOwnershipRequest";
    else if (msgId == SocialSym::PartyRespondToInvite) name = "PartyRespondToInvite";
    else if (msgId == SocialSym::PartyMemberUpdate) name = "PartyMemberUpdate";

    fprintf(stderr, "[NEVR.SOCIAL] SNS message: %s (hash=0x%llx, size=%llu)\n",
        name, (unsigned long long)msgId, (unsigned long long)msgSize);

    // Log hex payload for debugging
    if (msg && msgSize > 0 && msgSize <= 0x30) {
        const uint8_t* bytes = static_cast<const uint8_t*>(msg);
        char hex[256] = {0};
        for (uint64_t i = 0; i < msgSize && i < 48; i++) {
            sprintf(hex + i * 3, "%02x ", bytes[i]);
        }
        fprintf(stderr, "[NEVR.SOCIAL]   payload: %s\n", hex);
    }

    // Forward friend actions to Nakama
    if (msgSize == sizeof(SNSFriendsActionPayload) &&
        (msgId == SocialSym::FriendSendInviteRequest ||
         msgId == SocialSym::FriendRemoveRequest ||
         msgId == SocialSym::FriendActionRequest)) {
        const auto* payload = static_cast<const SNSFriendsActionPayload*>(msg);
        fprintf(stderr, "[NEVR.SOCIAL]   routing_id=0x%llx target_user_id=%llu\n",
            (unsigned long long)payload->routing_id, (unsigned long long)payload->target_user_id);
        HandleFriendAction(msgId, payload);
    }

    // Forward party actions to Nakama
    if (msgId == SocialSym::PartyKickRequest ||
        msgId == SocialSym::PartyPassOwnershipRequest ||
        msgId == SocialSym::PartyRespondToInvite ||
        msgId == SocialSym::PartyMemberUpdate) {
        HandlePartyAction(msgId, msg, msgSize);
    }
}

// ===================================================================================================
// Listener registration
// ===================================================================================================

// Register a broadcaster listener with a callback function.
static uint16_t ListenSocial(EchoVR::Broadcaster* broadcaster, EchoVR::SymbolId msgId, void* func) {
    EchoVR::DelegateProxy proxy = {};
    proxy.method[0] = 0xFFFFFFFFFFFFFFFF;  // DELEGATE_PROXY_INVALID_METHOD
    proxy.instance = s_netGame;
    proxy.proxyFunc = func;
    return s_BroadcasterListen(broadcaster, msgId, 1 /*TRUE*/, &proxy, 1 /*true*/);
}

// Register listeners for social SNS messages.
// Called when the game enters Lobby state (broadcaster is available by then).
static void RegisterSocialListeners() {
    EchoVR::Broadcaster* broadcaster = GetBroadcasterFromGame();
    if (!broadcaster) {
        fprintf(stderr, "[NEVR.SOCIAL] Broadcaster not available -- social listeners skipped\n");
        return;
    }

    int count = 0;

    // Register for friends outgoing messages
    if (ListenSocial(broadcaster, SocialSym::FriendSendInviteRequest, reinterpret_cast<void*>(OnSocialMessage))) count++;
    if (ListenSocial(broadcaster, SocialSym::FriendRemoveRequest, reinterpret_cast<void*>(OnSocialMessage))) count++;
    if (ListenSocial(broadcaster, SocialSym::FriendActionRequest, reinterpret_cast<void*>(OnSocialMessage))) count++;

    // Register for party outgoing messages
    if (ListenSocial(broadcaster, SocialSym::PartyKickRequest, reinterpret_cast<void*>(OnSocialMessage))) count++;
    if (ListenSocial(broadcaster, SocialSym::PartyPassOwnershipRequest, reinterpret_cast<void*>(OnSocialMessage))) count++;
    if (ListenSocial(broadcaster, SocialSym::PartyRespondToInvite, reinterpret_cast<void*>(OnSocialMessage))) count++;
    if (ListenSocial(broadcaster, SocialSym::PartyMemberUpdate, reinterpret_cast<void*>(OnSocialMessage))) count++;

    fprintf(stderr, "[NEVR.SOCIAL] Registered %d social message listeners\n", count);
}

// ===================================================================================================
// Friends list
// ===================================================================================================

// Fetch friends from Nakama and send updated ListResponse to pnsrad.
static void RefreshFriendsList() {
    if (!s_nakamaClient || !s_nakamaClient->IsAuthenticated()) {
        SendPlaceholderFriendsList();
        return;
    }

    std::vector<NevrFriend> friends;
    if (!s_nakamaClient->ListFriends(-1, friends)) {
        fprintf(stderr, "[NEVR.SOCIAL] Failed to fetch friends from Nakama\n");
        SendPlaceholderFriendsList();
        return;
    }

    // Count by state and online status
    uint32_t nonline = 0, noffline = 0, nbusy = 0, nsent = 0, nrecv = 0;
    for (const auto& f : friends) {
        switch (f.state) {
            case 0:  // friend
                if (f.online) nonline++;
                else noffline++;
                break;
            case 1: nsent++; break;     // invite sent
            case 2: nrecv++; break;     // invite received
            case 3: break;              // blocked -- don't count
        }
    }

    SNSFriendsListResponse response = {};
    response.nonline = nonline;
    response.noffline = noffline;
    response.nbusy = nbusy;
    response.nsent = nsent;
    response.nrecv = nrecv;

    SendSocialResponse(SocialSym::FriendListResponse, "SNSFriendsListResponse", &response, sizeof(response));

    fprintf(stderr, "[NEVR.SOCIAL] Friends list: online=%u offline=%u sent=%u recv=%u (total %zu)\n",
        nonline, noffline, nsent, nrecv, friends.size());
}

// Send a placeholder friends list response to pnsrad via the broadcaster.
// This populates pnsrad's internal friend counts so the friends UI renders.
static void SendPlaceholderFriendsList() {
    EchoVR::Broadcaster* broadcaster = GetBroadcasterFromGame();
    if (!broadcaster) {
        fprintf(stderr, "[NEVR.SOCIAL] Cannot send placeholder friends -- broadcaster unavailable\n");
        return;
    }

    // Send a ListResponse with placeholder counts
    SNSFriendsListResponse response = {};
    response.header = 0;          // SNS correlation
    response.nonline = 3;         // 3 online friends
    response.noffline = 1;        // 1 offline friend
    response.nbusy = 0;
    response.nsent = 0;
    response.nrecv = 1;           // 1 pending friend request
    response.reserved = 0;

    s_BroadcasterReceiveLocal(
        broadcaster,
        SocialSym::FriendListResponse,
        "SNSFriendsListResponse",
        &response,
        sizeof(response));

    fprintf(stderr, "[NEVR.SOCIAL] Sent placeholder friends list (online=%u, offline=%u, pending=%u)\n",
        response.nonline, response.noffline, response.nrecv);
}

// ===================================================================================================
// Config loading -- simple JSON key extraction from _local/config.json
// ===================================================================================================

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

// ===================================================================================================
// Public API
// ===================================================================================================

int SocialBridgeInit(uintptr_t base_addr) {
    s_baseAddr = base_addr;

    // Resolve BroadcasterListen
    void* listenAddr = nevr::ResolveVA(base_addr, nevr::addresses::VA_BROADCASTER_LISTEN);
    if (!listenAddr) {
        fprintf(stderr, "[NEVR.SOCIAL] Failed to resolve VA_BROADCASTER_LISTEN\n");
        return -1;
    }
    s_BroadcasterListen = reinterpret_cast<BroadcasterListenFn>(listenAddr);

    // Resolve BroadcasterReceiveLocalEvent
    void* recvAddr = nevr::ResolveVA(base_addr, nevr::addresses::VA_BROADCASTER_RECEIVE_LOCAL);
    if (!recvAddr) {
        fprintf(stderr, "[NEVR.SOCIAL] Failed to resolve VA_BROADCASTER_RECEIVE_LOCAL\n");
        return -2;
    }
    s_BroadcasterReceiveLocal = reinterpret_cast<BroadcasterReceiveLocalFn>(recvAddr);

    fprintf(stderr, "[NEVR.SOCIAL] Initialized (BroadcasterListen=%p, ReceiveLocal=%p)\n",
        reinterpret_cast<void*>(s_BroadcasterListen),
        reinterpret_cast<void*>(s_BroadcasterReceiveLocal));

    return 0;
}

void SocialBridgeOnStateChange(const NvrGameContext* ctx, uint32_t old_state, uint32_t new_state) {
    // Cache net_game from context for DelegateProxy.instance
    if (ctx->net_game) {
        s_netGame = ctx->net_game;
    }

    // Initialize social features when entering lobby (broadcaster is available by now).
    // NetGameState::Lobby == 5 (from echovr.h enum)
    if (!s_socialInitialized && new_state == 5) {
        fprintf(stderr, "[NEVR.SOCIAL] Entering lobby -- initializing social features\n");

        // Configure Nakama client from _local/config.json
        if (!s_nakamaClient) {
            s_nakamaClient = new NevrClient();
        }

        // Read config from _local/config.json using simple file I/O
        std::string configJson = nevr::LoadConfigFile("_local/config.json");
        if (!configJson.empty()) {
            std::string nakamaUrl = ExtractJsonString(configJson, "nevr_url");
            std::string nevrHttpKey = ExtractJsonString(configJson, "nevr_http_key");
            std::string nevrServerKey = ExtractJsonString(configJson, "nevr_server_key");

            if (!nakamaUrl.empty() && !nevrHttpKey.empty()) {
                s_nakamaClient->Configure(nakamaUrl, nevrHttpKey, nevrServerKey, "", "");

                // Try cached token from _local/auth.json first (written by token_auth plugin)
                std::string authJson = nevr::LoadConfigFile("_local/auth.json");
                if (!authJson.empty()) {
                    std::string cachedToken = ExtractJsonString(authJson, "token");
                    // Extract expiry (unquoted number)
                    uint64_t expiry = 0;
                    size_t epos = authJson.find("\"expiry\"");
                    if (epos != std::string::npos) {
                        epos = authJson.find(':', epos + 8);
                        if (epos != std::string::npos) {
                            epos++;
                            while (epos < authJson.size() && (authJson[epos] == ' ' || authJson[epos] == '\t'))
                                epos++;
                            expiry = std::strtoull(authJson.c_str() + epos, nullptr, 10);
                        }
                    }
                    if (!cachedToken.empty() && expiry > static_cast<uint64_t>(time(nullptr)) + 60) {
                        s_nakamaClient->SetToken(cachedToken, expiry);
                        fprintf(stderr, "[NEVR.SOCIAL] Using cached token from auth.json\n");
                    }
                }

                // Fall back to device auth if no cached token
                if (!s_nakamaClient->IsAuthenticated()) {
                    fprintf(stderr, "[NEVR.SOCIAL] Starting device code authentication (Discord OAuth)...\n");
                    if (s_nakamaClient->RunDeviceAuthFlow()) {
                        fprintf(stderr, "[NEVR.SOCIAL] Authenticated via Discord -- social features active\n");
                    } else {
                        fprintf(stderr, "[NEVR.SOCIAL] Auth failed -- social features using placeholders\n");
                    }
                }
            } else {
                fprintf(stderr, "[NEVR.SOCIAL] Nakama not configured (need nevr_url + nevr_http_key in _local/config.json)\n");
            }
        } else {
            fprintf(stderr, "[NEVR.SOCIAL] Could not read _local/config.json -- social features using placeholders\n");
        }

        RegisterSocialListeners();
        RefreshFriendsList();  // Uses Nakama if authenticated, else placeholder
        s_socialInitialized = true;
    }
}

void SocialBridgeShutdown() {
    if (s_nakamaClient) {
        delete s_nakamaClient;
        s_nakamaClient = nullptr;
    }
    s_BroadcasterListen = nullptr;
    s_BroadcasterReceiveLocal = nullptr;
    s_netGame = nullptr;
    s_baseAddr = 0;
    s_socialInitialized = false;
    fprintf(stderr, "[NEVR.SOCIAL] Shutdown complete\n");
}
