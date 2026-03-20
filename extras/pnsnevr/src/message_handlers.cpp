/*
 * pnsnevr - Message Handlers Implementation
 */

#include "message_handlers.h"
#include <Windows.h>
#include <cstring>

MessageHandlers::MessageHandlers(GameBridge* bridge, NakamaClient* client)
    : m_bridge(bridge)
    , m_client(client)
    , m_registered(false)
{
}

MessageHandlers::~MessageHandlers() {
    UnregisterAll();
}

bool MessageHandlers::RegisterAll() {
    if (m_registered) {
        return true;
    }
    
    if (!m_bridge || !m_client) {
        OutputDebugStringA("[pnsnevr] Cannot register handlers: bridge or client is null\n");
        return false;
    }
    
    OutputDebugStringA("[pnsnevr] Registering Nakama message handlers...\n");
    
    /*
     * Register handlers for incoming game messages that request Nakama operations.
     * These handlers receive messages from the game's internal systems and
     * forward them to Nakama.
     * 
     * Message flow:
     * 1. Game code calls internal messaging to request friends list
     * 2. Our handler receives the request
     * 3. We call NakamaClient to fetch data
     * 4. On response, we dispatch a new message back to the game
     */
    
    // Friends handlers
    if (!m_bridge->RegisterUdpHandler(
            MSG_NAKAMA_FRIENDS_LIST,
            OnFriendsRequest,
            this)) {
        OutputDebugStringA("[pnsnevr] Warning: Failed to register friends handler\n");
    }
    
    // Party handlers
    if (!m_bridge->RegisterUdpHandler(
            MSG_NAKAMA_PARTY_CREATED,
            OnPartyCreate,
            this)) {
        OutputDebugStringA("[pnsnevr] Warning: Failed to register party create handler\n");
    }
    
    if (!m_bridge->RegisterUdpHandler(
            MSG_NAKAMA_PARTY_JOINED,
            OnPartyJoin,
            this)) {
        OutputDebugStringA("[pnsnevr] Warning: Failed to register party join handler\n");
    }
    
    if (!m_bridge->RegisterUdpHandler(
            MSG_NAKAMA_PARTY_LEFT,
            OnPartyLeave,
            this)) {
        OutputDebugStringA("[pnsnevr] Warning: Failed to register party leave handler\n");
    }
    
    // Matchmaking handlers
    if (!m_bridge->RegisterUdpHandler(
            MSG_NAKAMA_MATCHMAKING_START,
            OnMatchmakingStart,
            this)) {
        OutputDebugStringA("[pnsnevr] Warning: Failed to register matchmaking start handler\n");
    }
    
    if (!m_bridge->RegisterUdpHandler(
            MSG_NAKAMA_MATCHMAKING_CANCEL,
            OnMatchmakingCancel,
            this)) {
        OutputDebugStringA("[pnsnevr] Warning: Failed to register matchmaking cancel handler\n");
    }
    
    m_registered = true;
    OutputDebugStringA("[pnsnevr] Message handlers registered successfully\n");
    return true;
}

void MessageHandlers::UnregisterAll() {
    if (!m_registered) {
        return;
    }
    
    OutputDebugStringA("[pnsnevr] Unregistering Nakama message handlers...\n");
    
    /*
     * TODO: Implement handler unregistration
     * 
     * The game's broadcaster system may not support unregistration.
     * Need to verify in Ghidra. For now, handlers remain registered
     * but will be no-ops after shutdown.
     */
    
    m_registered = false;
}

void MessageHandlers::ProcessCallbacks() {
    if (!m_client) {
        return;
    }
    
    // Pump Nakama client (processes any pending async operations)
    m_client->Tick();
    
    // Process any queued callbacks
    ProcessFriendsCallbacks();
    ProcessPartyCallbacks();
    ProcessMatchmakingCallbacks();
    ProcessPresenceCallbacks();
}

// ============================================================================
// Handler Implementations
// ============================================================================

void MessageHandlers::OnFriendsRequest(void* context, const uint8_t* data, size_t size) {
    MessageHandlers* self = static_cast<MessageHandlers*>(context);
    if (!self || !self->m_client) return;
    
    OutputDebugStringA("[pnsnevr] Friends list requested\n");
    
    // Initiate async friends list fetch
    self->m_client->GetFriendsList([self](const std::vector<NakamaFriend>& friends) {
        if (!self->m_bridge) return;
        
        char msg[128];
        sprintf_s(msg, "[pnsnevr] Received %zu friends\n", friends.size());
        OutputDebugStringA(msg);
        
        // Build response payload
        size_t payload_size = sizeof(NakamaFriendsListPayload) + 
                              friends.size() * sizeof(NakamaFriend);
        uint8_t* payload = new uint8_t[payload_size];
        
        NakamaFriendsListPayload* header = reinterpret_cast<NakamaFriendsListPayload*>(payload);
        header->friend_count = static_cast<uint32_t>(friends.size());
        
        // Copy friends data
        NakamaFriend* friend_data = reinterpret_cast<NakamaFriend*>(payload + sizeof(NakamaFriendsListPayload));
        for (size_t i = 0; i < friends.size(); ++i) {
            friend_data[i] = friends[i];
        }
        
        // Send to game
        self->m_bridge->SendMessage(MSG_NAKAMA_FRIENDS_LIST, payload, payload_size);
        
        delete[] payload;
    });
}

void MessageHandlers::OnPartyCreate(void* context, const uint8_t* data, size_t size) {
    MessageHandlers* self = static_cast<MessageHandlers*>(context);
    if (!self || !self->m_client) return;
    
    OutputDebugStringA("[pnsnevr] Party create requested\n");
    
    self->m_client->CreateParty([self](const NakamaParty& party) {
        if (!self->m_bridge) return;
        
        char msg[256];
        sprintf_s(msg, "[pnsnevr] Party created: %s\n", party.party_id);
        OutputDebugStringA(msg);
        
        // Build response payload
        NakamaPartyEventPayload payload{};
        strncpy_s(payload.party_id, party.party_id, sizeof(payload.party_id) - 1);
        strncpy_s(payload.leader_id, party.leader_id, sizeof(payload.leader_id) - 1);
        payload.member_count = party.member_count;
        payload.max_members = party.max_members;
        payload.event_type = MSG_NAKAMA_PARTY_CREATED;
        
        // Send to game
        self->m_bridge->SendMessage(MSG_NAKAMA_PARTY_CREATED, 
                                   reinterpret_cast<uint8_t*>(&payload),
                                   sizeof(payload));
    });
}

void MessageHandlers::OnPartyJoin(void* context, const uint8_t* data, size_t size) {
    MessageHandlers* self = static_cast<MessageHandlers*>(context);
    if (!self || !self->m_client) return;
    
    // Extract party ID from request
    const char* party_id = reinterpret_cast<const char*>(data);
    
    char msg[256];
    sprintf_s(msg, "[pnsnevr] Party join requested: %s\n", party_id);
    OutputDebugStringA(msg);
    
    self->m_client->JoinParty(party_id, [self](const NakamaParty& party) {
        if (!self->m_bridge) return;
        
        // Build response payload
        NakamaPartyEventPayload payload{};
        strncpy_s(payload.party_id, party.party_id, sizeof(payload.party_id) - 1);
        strncpy_s(payload.leader_id, party.leader_id, sizeof(payload.leader_id) - 1);
        payload.member_count = party.member_count;
        payload.max_members = party.max_members;
        payload.event_type = MSG_NAKAMA_PARTY_JOINED;
        
        // Send to game
        self->m_bridge->SendMessage(MSG_NAKAMA_PARTY_JOINED,
                                   reinterpret_cast<uint8_t*>(&payload),
                                   sizeof(payload));
    });
}

void MessageHandlers::OnPartyLeave(void* context, const uint8_t* data, size_t size) {
    MessageHandlers* self = static_cast<MessageHandlers*>(context);
    if (!self || !self->m_client) return;
    
    OutputDebugStringA("[pnsnevr] Party leave requested\n");
    
    self->m_client->LeaveParty([self]() {
        if (!self->m_bridge) return;
        
        OutputDebugStringA("[pnsnevr] Party left successfully\n");
        
        // Build response payload
        NakamaPartyEventPayload payload{};
        payload.event_type = MSG_NAKAMA_PARTY_LEFT;
        
        // Send to game
        self->m_bridge->SendMessage(MSG_NAKAMA_PARTY_LEFT,
                                   reinterpret_cast<uint8_t*>(&payload),
                                   sizeof(payload));
    });
}

void MessageHandlers::OnMatchmakingStart(void* context, const uint8_t* data, size_t size) {
    MessageHandlers* self = static_cast<MessageHandlers*>(context);
    if (!self || !self->m_client) return;
    
    // TODO: Parse matchmaking parameters from request data
    // For now, use empty query
    
    OutputDebugStringA("[pnsnevr] Matchmaking start requested\n");
    
    self->m_client->FindMatch(
        "*",   // Query: any match
        2,     // Min count
        8,     // Max count
        [self](const NakamaMatch& match) {
            if (!self->m_bridge) return;
            
            char msg[256];
            sprintf_s(msg, "[pnsnevr] Match found: %s at %s:%d\n", 
                     match.match_id, match.server_address, match.server_port);
            OutputDebugStringA(msg);
            
            // Build response payload
            NakamaMatchmakingEventPayload payload{};
            strncpy_s(payload.ticket, match.ticket, sizeof(payload.ticket) - 1);
            strncpy_s(payload.match_id, match.match_id, sizeof(payload.match_id) - 1);
            strncpy_s(payload.server_address, match.server_address, sizeof(payload.server_address) - 1);
            payload.server_port = match.server_port;
            payload.event_type = MSG_NAKAMA_MATCHMAKING_FOUND;
            payload.players_found = match.player_count;
            payload.players_needed = 0;
            
            // Send to game
            self->m_bridge->SendMessage(MSG_NAKAMA_MATCHMAKING_FOUND,
                                       reinterpret_cast<uint8_t*>(&payload),
                                       sizeof(payload));
        }
    );
}

void MessageHandlers::OnMatchmakingCancel(void* context, const uint8_t* data, size_t size) {
    MessageHandlers* self = static_cast<MessageHandlers*>(context);
    if (!self || !self->m_client) return;
    
    // Extract ticket from request
    const char* ticket = reinterpret_cast<const char*>(data);
    
    char msg[256];
    sprintf_s(msg, "[pnsnevr] Matchmaking cancel requested: %s\n", ticket);
    OutputDebugStringA(msg);
    
    self->m_client->CancelMatchmaking(ticket, [self]() {
        if (!self->m_bridge) return;
        
        OutputDebugStringA("[pnsnevr] Matchmaking cancelled\n");
        
        // Build response payload
        NakamaMatchmakingEventPayload payload{};
        payload.event_type = MSG_NAKAMA_MATCHMAKING_CANCEL;
        
        // Send to game
        self->m_bridge->SendMessage(MSG_NAKAMA_MATCHMAKING_CANCEL,
                                   reinterpret_cast<uint8_t*>(&payload),
                                   sizeof(payload));
    });
}

// ============================================================================
// Callback Processors
// ============================================================================

void MessageHandlers::ProcessFriendsCallbacks() {
    // Process friend presence updates
    // In production, NakamaClient would have a callback queue to process
}

void MessageHandlers::ProcessPartyCallbacks() {
    // Process party state updates
    // In production, NakamaClient would have a callback queue to process
}

void MessageHandlers::ProcessMatchmakingCallbacks() {
    // Process matchmaking ticket updates
    // In production, NakamaClient would have a callback queue to process
}

void MessageHandlers::ProcessPresenceCallbacks() {
    // Process presence updates from realtime socket
    // In production, NakamaClient would have a callback queue to process
}

// ============================================================================
// Public Factory
// ============================================================================

MessageHandlers* RegisterNakamaMessageHandlers(GameBridge* bridge, NakamaClient* client) {
    MessageHandlers* handlers = new MessageHandlers(bridge, client);
    
    if (!handlers->RegisterAll()) {
        delete handlers;
        return nullptr;
    }
    
    return handlers;
}
