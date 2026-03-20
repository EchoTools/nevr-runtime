/*
 * pnsnevr - Nakama Client Implementation
 * 
 * PROTOTYPE IMPLEMENTATION
 * 
 * This is a mock implementation for testing. Production code should:
 * 1. Build nakama-cpp library (see nakama-cpp/README.md)
 * 2. Link against nakama-sdk.lib
 * 3. Replace mock implementations with real nakama-cpp API calls
 */

#include "nakama_client.h"
#include "config.h"
#include <windows.h>
#include <cstdio>

// TODO: When nakama-cpp is built, uncomment this:
// #include <nakama-cpp/Nakama.h>

struct NakamaClient::Impl {
    // TODO: Replace with real nakama-cpp client
    // Nakama::NClientPtr client;
    // Nakama::NSessionPtr session;
    // Nakama::NRtClientPtr rtClient;
    
    // Mock state
    bool connected = false;
    std::string authToken;
};

NakamaClient::NakamaClient(const NakamaConfig& config)
    : m_config(config)
    , m_impl(std::make_unique<Impl>())
    , m_authenticated(false)
{
}

NakamaClient::~NakamaClient()
{
    Disconnect();
}

bool NakamaClient::Initialize()
{
    char logBuf[512];
    snprintf(logBuf, sizeof(logBuf), 
             "[pnsnevr] Initializing Nakama client: %s:%d\n",
             m_config.api_endpoint.c_str(),
             m_config.http_port);
    OutputDebugStringA(logBuf);
    
    /* TODO: Replace with real nakama-cpp initialization:
    
    Nakama::NClientParameters params;
    params.serverKey = m_config.server_key;
    params.host = m_config.api_endpoint;
    params.port = m_config.http_port;
    
    m_impl->client = Nakama::createDefaultClient(params);
    if (!m_impl->client) {
        OutputDebugStringA("[pnsnevr] Failed to create Nakama client\n");
        return false;
    }
    
    */
    
    // Mock: Always succeed
    OutputDebugStringA("[pnsnevr] MOCK: Client initialized (no real connection)\n");
    return true;
}

void NakamaClient::Tick()
{
    /* TODO: Replace with real nakama-cpp tick:
    
    if (m_impl->client) {
        m_impl->client->tick();
    }
    if (m_impl->rtClient) {
        m_impl->rtClient->tick();
    }
    
    */
    
    // Process queued callbacks
    ProcessCallbacks();
}

void NakamaClient::Disconnect()
{
    OutputDebugStringA("[pnsnevr] Disconnecting from Nakama\n");
    
    /* TODO: Replace with real disconnect:
    
    if (m_impl->rtClient) {
        m_impl->rtClient->disconnect();
        m_impl->rtClient.reset();
    }
    if (m_impl->client) {
        m_impl->client->disconnect();
    }
    
    */
    
    m_impl->connected = false;
    m_authenticated = false;
}

bool NakamaClient::AuthenticateDevice(const std::string& deviceId)
{
    char logBuf[512];
    snprintf(logBuf, sizeof(logBuf), 
             "[pnsnevr] Authenticating with device ID: %s\n",
             deviceId.c_str());
    OutputDebugStringA(logBuf);
    
    /* TODO: Replace with real nakama-cpp authentication:
    
    bool done = false;
    bool success = false;
    
    m_impl->client->authenticateDevice(
        deviceId,
        std::nullopt,  // username
        std::optional<bool>(true),  // create
        {},  // vars
        [this, &done, &success](Nakama::NSessionPtr session) {
            m_impl->session = session;
            m_userId = session->getUserId();
            m_username = session->getUsername();
            m_authenticated = true;
            done = true;
            success = true;
            OutputDebugStringA("[pnsnevr] Authentication successful\n");
        },
        [&done, &success](const Nakama::NError& error) {
            char buf[256];
            snprintf(buf, sizeof(buf), "[pnsnevr] Auth error: %s\n", error.message.c_str());
            OutputDebugStringA(buf);
            done = true;
            success = false;
        }
    );
    
    // Block for initial auth (production should be async)
    while (!done) {
        m_impl->client->tick();
        Sleep(10);
    }
    
    return success;
    
    */
    
    // Mock: Generate fake user ID and succeed
    m_userId = "mock-user-" + deviceId.substr(0, 8);
    m_username = "MockUser";
    m_authenticated = true;
    m_impl->connected = true;
    
    snprintf(logBuf, sizeof(logBuf), 
             "[pnsnevr] MOCK: Authenticated as %s (%s)\n",
             m_username.c_str(),
             m_userId.c_str());
    OutputDebugStringA(logBuf);
    
    return true;
}

bool NakamaClient::AuthenticateCustom(const std::string& customId)
{
    // Similar to AuthenticateDevice, but uses custom ID
    OutputDebugStringA("[pnsnevr] MOCK: AuthenticateCustom\n");
    m_userId = "mock-custom-" + customId.substr(0, 8);
    m_username = "MockCustomUser";
    m_authenticated = true;
    return true;
}

bool NakamaClient::AuthenticateEmail(const std::string& email, const std::string& password)
{
    OutputDebugStringA("[pnsnevr] MOCK: AuthenticateEmail\n");
    m_userId = "mock-email-user";
    m_username = email.substr(0, email.find('@'));
    m_authenticated = true;
    return true;
}

bool NakamaClient::IsAuthenticated() const
{
    return m_authenticated;
}

std::string NakamaClient::GetUserId() const
{
    return m_userId;
}

std::string NakamaClient::GetUsername() const
{
    return m_username;
}

void NakamaClient::GetFriendsList(FriendsCallback callback, ErrorCallback errorCallback)
{
    OutputDebugStringA("[pnsnevr] GetFriendsList\n");
    
    /* TODO: Replace with real nakama-cpp:
    
    m_impl->client->listFriends(
        m_impl->session,
        std::nullopt,  // limit
        std::nullopt,  // state
        "",  // cursor
        [this, callback](Nakama::NFriendListPtr list) {
            std::vector<NakamaFriend> friends;
            for (const auto& f : list->friends) {
                NakamaFriend nf;
                nf.userId = f.user.id;
                nf.username = f.user.username;
                nf.displayName = f.user.displayName;
                nf.avatarUrl = f.user.avatarUrl;
                nf.state = static_cast<int>(f.state);
                nf.online = f.user.online;
                friends.push_back(nf);
            }
            QueueCallback([callback, friends]() { callback(friends); });
        },
        [errorCallback](const Nakama::NError& error) {
            if (errorCallback) {
                QueueCallback([errorCallback, error]() { 
                    errorCallback(error.code, error.message); 
                });
            }
        }
    );
    
    */
    
    // Mock: Return empty friends list
    QueueCallback([callback]() {
        std::vector<NakamaFriend> friends;
        // Add a mock friend for testing
        NakamaFriend mockFriend;
        mockFriend.userId = "mock-friend-001";
        mockFriend.username = "TestFriend";
        mockFriend.displayName = "Test Friend";
        mockFriend.state = 0;
        mockFriend.online = true;
        friends.push_back(mockFriend);
        callback(friends);
    });
}

void NakamaClient::AddFriend(const std::string& userId, ErrorCallback errorCallback)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "[pnsnevr] AddFriend: %s\n", userId.c_str());
    OutputDebugStringA(logBuf);
    
    // TODO: Implement with nakama-cpp client->addFriends
}

void NakamaClient::RemoveFriend(const std::string& userId, ErrorCallback errorCallback)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "[pnsnevr] RemoveFriend: %s\n", userId.c_str());
    OutputDebugStringA(logBuf);
    
    // TODO: Implement with nakama-cpp client->deleteFriends
}

void NakamaClient::BlockFriend(const std::string& userId, ErrorCallback errorCallback)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "[pnsnevr] BlockFriend: %s\n", userId.c_str());
    OutputDebugStringA(logBuf);
    
    // TODO: Implement with nakama-cpp client->blockFriends
}

void NakamaClient::UpdatePresence(const std::string& status)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "[pnsnevr] UpdatePresence: %s\n", status.c_str());
    OutputDebugStringA(logBuf);
    
    // TODO: Implement with nakama-cpp rtClient->updateStatus
}

void NakamaClient::SubscribePresence(const std::vector<std::string>& userIds,
                                     PresenceCallback callback,
                                     ErrorCallback errorCallback)
{
    OutputDebugStringA("[pnsnevr] SubscribePresence\n");
    
    // TODO: Implement with nakama-cpp rtClient presence subscriptions
}

void NakamaClient::CreateParty(bool open, int maxSize, 
                               PartyCallback callback, 
                               ErrorCallback errorCallback)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), 
             "[pnsnevr] CreateParty: open=%d maxSize=%d\n", open, maxSize);
    OutputDebugStringA(logBuf);
    
    /* TODO: Implement with nakama-cpp:
    
    m_impl->rtClient->createParty(
        open, maxSize,
        [callback](const Nakama::NParty& party) {
            NakamaParty p;
            p.partyId = party.partyId;
            p.leaderId = party.leader.userId;
            p.open = party.open;
            p.maxSize = party.maxSize;
            for (const auto& m : party.presences) {
                p.memberIds.push_back(m.userId);
            }
            callback(p);
        }
    );
    
    */
    
    // Mock: Create a fake party
    QueueCallback([callback]() {
        NakamaParty party;
        party.partyId = "mock-party-001";
        party.leaderId = "mock-user";
        party.open = true;
        party.maxSize = 4;
        party.memberIds.push_back("mock-user");
        callback(party);
    });
}

void NakamaClient::JoinParty(const std::string& partyId, 
                             PartyCallback callback, 
                             ErrorCallback errorCallback)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "[pnsnevr] JoinParty: %s\n", partyId.c_str());
    OutputDebugStringA(logBuf);
    
    // TODO: Implement with nakama-cpp rtClient->joinParty
}

void NakamaClient::LeaveParty(const std::string& partyId, ErrorCallback errorCallback)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "[pnsnevr] LeaveParty: %s\n", partyId.c_str());
    OutputDebugStringA(logBuf);
    
    // TODO: Implement with nakama-cpp rtClient->leaveParty
}

void NakamaClient::SendPartyData(const std::string& partyId, const std::vector<uint8_t>& data)
{
    // TODO: Implement with nakama-cpp rtClient->sendPartyData
}

void NakamaClient::FindMatch(const std::string& query, int minPlayers, int maxPlayers,
                             MatchCallback callback, ErrorCallback errorCallback)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), 
             "[pnsnevr] FindMatch: query=%s min=%d max=%d\n", 
             query.c_str(), minPlayers, maxPlayers);
    OutputDebugStringA(logBuf);
    
    /* TODO: Implement with nakama-cpp:
    
    Nakama::NStringMap stringProps;
    Nakama::NStringDoubleMap numericProps;
    
    m_impl->rtClient->addMatchmaker(
        minPlayers, maxPlayers,
        query,
        stringProps,
        numericProps,
        [callback](const Nakama::NMatchmakerTicket& ticket) {
            NakamaMatch match;
            match.ticketId = ticket.ticket;
            match.status = "searching";
            callback(match);
        }
    );
    
    // Also set up matchmaker matched callback:
    // listener.setMatchmakerMatchedCallback([](const Nakama::NMatchmakerMatched& matched) { ... });
    
    */
    
    // Mock: Return a searching status
    QueueCallback([callback]() {
        NakamaMatch match;
        match.ticketId = "mock-ticket-001";
        match.status = "searching";
        callback(match);
    });
}

void NakamaClient::CancelMatchmaking(const std::string& ticketId, ErrorCallback errorCallback)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "[pnsnevr] CancelMatchmaking: %s\n", ticketId.c_str());
    OutputDebugStringA(logBuf);
    
    // TODO: Implement with nakama-cpp rtClient->removeMatchmaker
}

void NakamaClient::CallRpc(const std::string& rpcId, const std::string& payload,
                           std::function<void(const std::string&)> callback,
                           ErrorCallback errorCallback)
{
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "[pnsnevr] CallRpc: %s\n", rpcId.c_str());
    OutputDebugStringA(logBuf);
    
    /* TODO: Implement with nakama-cpp:
    
    m_impl->client->rpc(
        m_impl->session,
        rpcId,
        payload,
        [callback](const Nakama::NRpc& rpc) {
            callback(rpc.payload);
        },
        [errorCallback](const Nakama::NError& error) {
            if (errorCallback) {
                errorCallback(error.code, error.message);
            }
        }
    );
    
    */
    
    // Mock: Return empty response
    QueueCallback([callback]() {
        callback("{}");
    });
}

void NakamaClient::QueueCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_callbackQueue.push(callback);
}

void NakamaClient::ProcessCallbacks()
{
    std::queue<std::function<void()>> localQueue;
    
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        std::swap(localQueue, m_callbackQueue);
    }
    
    while (!localQueue.empty()) {
        auto callback = localQueue.front();
        localQueue.pop();
        callback();
    }
}
