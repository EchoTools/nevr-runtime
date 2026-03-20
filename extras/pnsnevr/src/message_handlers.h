/*
 * pnsnevr - Message Handlers
 *
 * Registers handlers for Nakama events and bridges them to the game's
 * internal message system.
 */

#pragma once

#include "game_bridge.h"
#include "nakama_client.h"

/*
 * Message type IDs for Nakama events.
 * These are custom message types that will be injected into the game's
 * message handling system.
 *
 * We use IDs in the 0x8000+ range to avoid collision with existing game messages.
 */
enum NakamaMessageTypes : uint32_t {
  // Friends system
  MSG_NAKAMA_FRIENDS_LIST = 0x8001,
  MSG_NAKAMA_FRIEND_REQUEST = 0x8002,
  MSG_NAKAMA_FRIEND_ACCEPTED = 0x8003,
  MSG_NAKAMA_FRIEND_REMOVED = 0x8004,
  MSG_NAKAMA_FRIEND_ONLINE = 0x8005,
  MSG_NAKAMA_FRIEND_OFFLINE = 0x8006,

  // Party system
  MSG_NAKAMA_PARTY_CREATED = 0x8010,
  MSG_NAKAMA_PARTY_JOINED = 0x8011,
  MSG_NAKAMA_PARTY_LEFT = 0x8012,
  MSG_NAKAMA_PARTY_MEMBER_JOIN = 0x8013,
  MSG_NAKAMA_PARTY_MEMBER_LEAVE = 0x8014,
  MSG_NAKAMA_PARTY_LEADER_CHANGE = 0x8015,
  MSG_NAKAMA_PARTY_INVITE = 0x8016,

  // Matchmaking
  MSG_NAKAMA_MATCHMAKING_START = 0x8020,
  MSG_NAKAMA_MATCHMAKING_UPDATE = 0x8021,
  MSG_NAKAMA_MATCHMAKING_FOUND = 0x8022,
  MSG_NAKAMA_MATCHMAKING_CANCEL = 0x8023,
  MSG_NAKAMA_MATCHMAKING_ERROR = 0x8024,

  // Presence/Status
  MSG_NAKAMA_PRESENCE_UPDATE = 0x8030,
  MSG_NAKAMA_STATUS_UPDATE = 0x8031,

  // General
  MSG_NAKAMA_CONNECTION_STATUS = 0x8040,
  MSG_NAKAMA_ERROR = 0x8041,
  MSG_NAKAMA_RPC_RESPONSE = 0x8042,
};

/*
 * Message payloads for Nakama messages
 */
#pragma pack(push, 1)

struct NakamaFriendsListPayload {
  uint32_t friend_count;
  // Followed by friend_count * NakamaFriend entries
};

struct NakamaFriendEventPayload {
  char user_id[64];
  char username[32];
  uint32_t event_type;  // MSG_NAKAMA_FRIEND_*
};

struct NakamaPartyEventPayload {
  char party_id[64];
  char leader_id[64];
  uint32_t member_count;
  uint32_t max_members;
  uint32_t event_type;  // MSG_NAKAMA_PARTY_*
};

struct NakamaMatchmakingEventPayload {
  char ticket[128];
  char match_id[64];
  char server_address[256];
  uint16_t server_port;
  uint32_t event_type;  // MSG_NAKAMA_MATCHMAKING_*
  uint32_t players_found;
  uint32_t players_needed;
};

struct NakamaPresenceEventPayload {
  char user_id[64];
  char username[32];
  char status[128];
  bool is_online;
};

struct NakamaConnectionStatusPayload {
  bool is_connected;
  uint32_t error_code;
  char error_message[256];
};

#pragma pack(pop)

/*
 * Message handler context
 */
class MessageHandlers {
 public:
  MessageHandlers(GameBridge* bridge, NakamaClient* client);
  ~MessageHandlers();

  /*
   * Register all Nakama message handlers with the game.
   */
  bool RegisterAll();

  /*
   * Unregister all handlers.
   */
  void UnregisterAll();

  /*
   * Process Nakama callbacks and dispatch to game message system.
   * Called from DLL_Tick.
   */
  void ProcessCallbacks();

 private:
  GameBridge* m_bridge;
  NakamaClient* m_client;
  bool m_registered;

  // Handler implementations
  static void OnFriendsRequest(void* context, const uint8_t* data, size_t size);
  static void OnPartyCreate(void* context, const uint8_t* data, size_t size);
  static void OnPartyJoin(void* context, const uint8_t* data, size_t size);
  static void OnPartyLeave(void* context, const uint8_t* data, size_t size);
  static void OnMatchmakingStart(void* context, const uint8_t* data, size_t size);
  static void OnMatchmakingCancel(void* context, const uint8_t* data, size_t size);

  // Callback processors
  void ProcessFriendsCallbacks();
  void ProcessPartyCallbacks();
  void ProcessMatchmakingCallbacks();
  void ProcessPresenceCallbacks();
};

/*
 * Register Nakama message handlers with the game.
 * This is the main entry point called from DLL_Initialize.
 *
 * @param bridge Pointer to initialized GameBridge
 * @param client Pointer to initialized NakamaClient
 * @return MessageHandlers instance (caller owns)
 */
MessageHandlers* RegisterNakamaMessageHandlers(GameBridge* bridge, NakamaClient* client);
