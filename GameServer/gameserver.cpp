#include "gameserver.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "constants.h"
#include "echovr.h"
#include "echovrunexported.h"
#include "messages.h"
#include "pch.h"
#include "rtapi/realtime_v1.pb.h"

using namespace GameServer;

// Logging wrapper for game's log system
void Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  va_list args;
  va_start(args, format);
  EchoVR::WriteLog(level, 0, format, args);
  va_end(args);
}

// Subscribe to internal broadcaster (UDP) events
uint16_t ListenForBroadcasterMessage(GameServerLib* self, EchoVR::SymbolId msgId, BOOL isMsgReliable, VOID* func) {
  EchoVR::DelegateProxy proxy = {};
  proxy.method[0] = DELEGATE_PROXY_INVALID_METHOD;
  proxy.instance = static_cast<VOID*>(self);
  proxy.proxyFunc = func;

  auto* lobby = self->GetContext().GetLobby();
  if (!lobby || !lobby->broadcaster) return 0;

  return EchoVR::BroadcasterListen(lobby->broadcaster, msgId, isMsgReliable, &proxy, true);
}

// Subscribe to TCP broadcaster (websocket) events
uint16_t ListenForTcpBroadcasterMessage(GameServerLib* self, EchoVR::SymbolId msgId, VOID* func) {
  EchoVR::DelegateProxy proxy = {};
  proxy.method[0] = DELEGATE_PROXY_INVALID_METHOD;
  proxy.instance = static_cast<VOID*>(self);
  proxy.proxyFunc = func;

  auto* lobby = self->GetContext().GetLobby();
  if (!lobby || !lobby->tcpBroadcaster) return 0;

  return EchoVR::TcpBroadcasterListen(lobby->tcpBroadcaster, msgId, 0, 0, 0, &proxy, true);
}

// Symbol ID for NEVRProtobufMessageV1 (binary protobuf)
constexpr EchoVR::SymbolId SYM_PROTOBUF_MSG = 0x9ee5107d9e29fd63ULL;

// Send a protobuf Envelope to ServerDB as binary
bool SendProtobufEnvelope(GameServerLib* self, const realtime::Envelope& envelope) {
  auto* tcp = self->GetContext().GetTcpBroadcaster();
  if (!tcp) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Cannot send protobuf: TCP broadcaster unavailable");
    return false;
  }

  // Serialize envelope to binary
  std::string binaryData;
  if (!envelope.SerializeToString(&binaryData)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to serialize protobuf to binary");
    return false;
  }

  // Log the message type being sent
  const char* msgType = "unknown";
  switch (envelope.message_case()) {
    case realtime::Envelope::kGameServerRegistration:
      msgType = "GameServerRegistration";
      break;
    case realtime::Envelope::kLobbySessionEvent:
      msgType = "LobbySessionEvent";
      break;
    case realtime::Envelope::kLobbyEntrantConnected:
      msgType = "LobbyEntrantConnected";
      break;
    case realtime::Envelope::kLobbyEntrantRemoved:
      msgType = "LobbyEntrantRemoved";
      break;
    case realtime::Envelope::kGameServerSaveLoadout:
      msgType = "GameServerSaveLoadout";
      break;
    default:
      break;
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Sending protobuf: %s (%zu bytes)", msgType, binaryData.size());

  // Send via TCP with protobuf binary symbol
  auto peer = self->GetContext().GetServerDbPeer();
  tcp->SendToPeer(peer, SYM_PROTOBUF_MSG, nullptr, 0, const_cast<char*>(binaryData.c_str()), binaryData.size());

  return true;
}

// Helper to convert GUID to UUID string format
static std::string GuidToUuidString(const GUID& guid) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid.Data1, guid.Data2, guid.Data3,
           guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
           guid.Data4[7]);
  return buf;
}

// Helper to convert IPv4 address (uint32_t) to string
static std::string Ipv4ToString(uint32_t ip) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 0) & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
  return buf;
}

// Extract slot index from message payload
SlotInfo ExtractSlotIndex(const void* msg, uint64_t msgSize) {
  SlotInfo info = {0, 0};
  if (!msg || msgSize < sizeof(uint32_t)) return info;

  uint32_t packed = 0;
  std::memcpy(&packed, msg, sizeof(uint32_t));

  info.slot = static_cast<uint16_t>(packed & SLOT_INDEX_MASK);
  info.genId = static_cast<uint16_t>((packed >> SLOT_GEN_SHIFT) & SLOT_INDEX_MASK);
  return info;
}

// --- TCP Broadcaster Callbacks ---

// Handle incoming protobuf messages from Nakama
void OnTcpMsgProtobuf(GameServerLib* self, VOID*, EchoVR::TcpPeer, VOID* msg, VOID*, UINT64 msgSize) {
  if (!msg || msgSize == 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Received empty protobuf message");
    return;
  }

  // Parse the protobuf Envelope
  realtime::Envelope envelope;
  if (!envelope.ParseFromArray(msg, static_cast<int>(msgSize))) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to parse protobuf Envelope");
    return;
  }

  auto* broadcaster = self->GetContext().GetBroadcaster();

  // Dispatch based on message type
  switch (envelope.message_case()) {
    case realtime::Envelope::kGameServerRegistrationSuccess: {
      const auto& regSuccess = envelope.game_server_registration_success();
      Log(EchoVR::LogLevel::Info,
          "[NEVR.GAMESERVER] Received registration success via protobuf: serverId=%llu, externalIP=%s",
          regSuccess.server_id(), regSuccess.external_ip_address().c_str());
      self->GetContext().SetRegistered(true);

      // Encode protobuf to binary format for the game
      auto encoded = EncodeRegistrationSuccess(regSuccess);

      // Forward as legacy event with properly encoded binary data
      if (broadcaster) {
        EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyRegistrationSuccess, "SNSLobbyRegistrationSuccess",
                                             const_cast<uint8_t*>(encoded.ptr()), encoded.size());
      }
      break;
    }

    case realtime::Envelope::kLobbySessionCreate: {
      const auto& sessionCreate = envelope.lobby_session_create();
      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Received session create via protobuf: session=%s",
          sessionCreate.lobby_session_id().c_str());

      // Update session state
      SessionState state = self->GetContext().GetSessionState();
      state.lobbySessionId = sessionCreate.lobby_session_id();
      self->GetContext().UpdateSessionState(state);
      self->GetContext().StartSession();

      // Encode protobuf to binary format for the game
      auto encoded = EncodeLobbySessionCreate(sessionCreate);
      if (encoded.size() == 0) {
        Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to encode LobbySessionCreate to binary");
        break;
      }

      // Forward as legacy event with properly encoded binary data
      if (broadcaster) {
        EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyStartSessionV4, "SNSLobbyStartSessionv4",
                                             const_cast<uint8_t*>(encoded.ptr()), encoded.size());
      }
      break;
    }

    case realtime::Envelope::kLobbyEntrantsAccept: {
      const auto& accept = envelope.lobby_entrants_accept();
      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Received entrants accept via protobuf: count=%d",
          accept.entrant_ids_size());

      // Encode protobuf to binary format for the game
      auto encoded = EncodeLobbyEntrantsAccept(accept);

      // Forward as legacy event with properly encoded binary data
      if (broadcaster) {
        EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyAcceptPlayersSuccessV2,
                                             "SNSLobbyAcceptPlayersSuccessv2", const_cast<uint8_t*>(encoded.ptr()),
                                             encoded.size());
      }
      break;
    }

    case realtime::Envelope::kLobbyEntrantReject: {
      const auto& reject = envelope.lobby_entrant_reject();
      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Received entrants reject via protobuf: count=%d, code=%d",
          reject.entrant_ids_size(), reject.code());

      // Encode protobuf to binary format for the game
      auto encoded = EncodeLobbyEntrantsReject(reject);

      // Forward as legacy event with properly encoded binary data
      if (broadcaster) {
        EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyAcceptPlayersFailureV2,
                                             "SNSLobbyAcceptPlayersFailurev2", const_cast<uint8_t*>(encoded.ptr()),
                                             encoded.size());
      }
      break;
    }

    case realtime::Envelope::kLobbySessionSuccessV5: {
      const auto& success = envelope.lobby_session_success_v5();
      Log(EchoVR::LogLevel::Info,
          "[NEVR.GAMESERVER] Received LobbySessionSuccessV5 via protobuf: lobby=%s, endpoint=%s, slot=%u",
          success.lobby_id().c_str(), success.endpoint().c_str(), success.user_slot());

      // Encode protobuf to binary format for the game
      auto encoded = EncodeLobbySessionSuccessV5(success);
      if (encoded.size() == 0) {
        Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to encode LobbySessionSuccessV5 to binary");
        break;
      }

      // Forward as legacy event with properly encoded binary data
      if (broadcaster) {
        EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbySessionSuccessV5, "SNSLobbySessionSuccessv5",
                                             const_cast<uint8_t*>(encoded.ptr()), encoded.size());
      }
      break;
    }

    case realtime::Envelope::kError: {
      const auto& error = envelope.error();
      Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Received error via protobuf: code=%d, msg=%s", error.code(),
          error.message().c_str());
      break;
    }

    default:
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Received unhandled protobuf message type: %d",
          envelope.message_case());
      break;
  }
}

// --- Internal Broadcaster Callbacks ---

void OnMsgSessionStarting(GameServerLib* self, VOID*, VOID*, UINT64, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Session starting");
}

void OnMsgSessionError(GameServerLib* self, VOID*, VOID*, UINT64, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Session error encountered");
}

// Helper to populate a protobuf LoadoutSlot from a game LoadoutSlot
static void PopulateLoadoutSlot(realtime::LoadoutSlot* proto, const EchoVR::LoadoutSlot* slot) {
  proto->set_selectionmode(slot->selectionmode);
  proto->set_banner(slot->banner);
  proto->set_booster(slot->booster);
  proto->set_bracer(slot->bracer);
  proto->set_chassis(slot->chassis);
  proto->set_decal(slot->decal);
  proto->set_decal_body(slot->decal_body);
  proto->set_emissive(slot->emissive);
  proto->set_emote(slot->emote);
  proto->set_secondemote(slot->secondemote);
  proto->set_goal_fx(slot->goal_fx);
  proto->set_medal(slot->medal);
  proto->set_pattern(slot->pattern);
  proto->set_pattern_body(slot->pattern_body);
  proto->set_pip(slot->pip);
  proto->set_tag(slot->tag);
  proto->set_tint(slot->tint);
  proto->set_tint_alignment_a(slot->tint_alignment_a);
  proto->set_tint_alignment_b(slot->tint_alignment_b);
  proto->set_tint_body(slot->tint_body);
  proto->set_title(slot->title);
}

// Helper to populate a protobuf LoadoutEntry from a game LoadoutEntry
static void PopulateLoadoutEntry(realtime::LoadoutEntry* proto, const EchoVR::LoadoutEntry* entry) {
  proto->set_bodytype(entry->bodytype);
  proto->set_teamid(entry->teamid);
  proto->set_airole(entry->airole);
  proto->set_xf(entry->xf);
  PopulateLoadoutSlot(proto->mutable_loadout(), &entry->loadout);
}

// LoadoutInstance structure at game + 0x51420 + (slot * 0x40)
// This is a pointer array, NOT direct data!
struct LoadoutInstanceHeader {
  uint64_t* instancesPtr;  // +0x00: Pointer to array of LoadoutInstance
  uint64_t _pad08;         // +0x08
  uint64_t _pad10;         // +0x10
  uint64_t _pad18;         // +0x18
  uint64_t _pad20;         // +0x20
  uint64_t _pad28;         // +0x28
  uint64_t instanceCount;  // +0x30: Number of loadout instances
  uint16_t loadoutNumber;  // +0x38
  uint16_t validation;     // +0x3A
  uint32_t flags;          // +0x3C
};

// Each loadout instance (0x40 bytes) in the instances array
struct LoadoutInstance {
  EchoVR::SymbolId instanceName;  // +0x00: e.g., "rwd" hash
  uint64_t* itemsArrayPtr;        // +0x08: Pointer to item pairs (slotType, equippedItem)
  uint64_t _pad10;                // +0x10
  uint64_t _pad18;                // +0x18
  uint64_t _pad20;                // +0x20
  uint64_t itemCount;             // +0x28: Number of items in the array
  uint64_t _pad30;                // +0x30
  uint64_t _pad38;                // +0x38
};

// Each item is a pair: (slotType SymbolId, equippedItem SymbolId)
struct LoadoutItem {
  EchoVR::SymbolId slotType;      // e.g., tint_body = 0xd90c85db5e5629ed
  EchoVR::SymbolId equippedItem;  // e.g., rwd_tint_0019 = 0x74d228d09dc5dd8f
};

// Check if a value looks like a valid SymbolId (not a pointer or garbage)
static bool IsValidSymbolId(uint64_t value) {
  // Valid SymbolIds have high bits set and don't look like pointers
  // Pointers on this system start with 0x00007F... or 0x000000...
  // Garbage values like 0xFEFEFEFE or 0x0000 are also invalid
  if (value == 0) return false;
  if (value < 0x0100000000000000ULL) return false;  // Too small, likely garbage or pointer
  if ((value >> 48) == 0x7F3F) return false;        // Looks like a heap pointer
  return true;
}

void OnMsgSaveLoadoutRequest(GameServerLib* self, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  if (!msg || msgSize < MIN_LOADOUT_MSG_SIZE) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Invalid message size: %llu", msgSize);
    return;
  }

  auto slot = ExtractSlotIndex(msg, msgSize);

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Slot=%u, GenId=%u, PayloadSize=%llu", slot.slot,
      slot.genId, msgSize);

  if (slot.slot >= MAX_PLAYER_SLOTS) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Invalid slot index: %u", slot.slot);
    return;
  }

  // Log player info
  auto* entrant = self->GetContext().GetEntrant(slot.slot);
  if (entrant) {
    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Player: %s (%s)", entrant->displayName,
        entrant->uniqueName);
  }

  // Access loadout via global: g_GameContext + 0x8518 = CR15NetGame
  // Then: CR15NetGame + 0x51420 + (slot * 0x40) = pointer to instances array
  //       CR15NetGame + 0x51450 + (slot * 0x40) = instance count
  //
  // g_GameContext is at static address 0x1420a0478, offset from base = 0x20a0478
  constexpr uint64_t GAME_CONTEXT_OFFSET = 0x20a0478;
  constexpr uint64_t NETGAME_OFFSET = 0x8518;

  CHAR* baseAddr = EchoVR::g_GameBaseAddress;
  if (!baseAddr) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [SAVE_LOADOUT] No game base address");
    return;
  }

  // Get g_GameContext
  VOID** contextPtr = reinterpret_cast<VOID**>(baseAddr + GAME_CONTEXT_OFFSET);
  VOID* gameContext = *contextPtr;

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Base=%p, Context=%p", baseAddr, gameContext);

  if (gameContext) {
    // Get CR15NetGame from context + 0x8518
    CHAR* contextBase = reinterpret_cast<CHAR*>(gameContext);
    VOID* netGame = *reinterpret_cast<VOID**>(contextBase + NETGAME_OFFSET);

    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_LOADOUT] NetGame=%p (from context+0x%X)", netGame,
        NETGAME_OFFSET);

    if (netGame) {
      CHAR* gameBase = reinterpret_cast<CHAR*>(netGame);
      uint16_t playerSlot = slot.slot;

      if (playerSlot < 16) {
        // Read jersey number from gameBase + 0x51458 + (playerSlot * 0x40)
        uint16_t jerseyNumber = *reinterpret_cast<uint16_t*>(gameBase + 0x51458 + (playerSlot * 0x40));
        // Read the loadout header for this slot
        CHAR* headerAddr = gameBase + 0x51420 + (playerSlot * 0x40);
        LoadoutInstanceHeader* header = reinterpret_cast<LoadoutInstanceHeader*>(headerAddr);

        // Read instance count from +0x30 offset within the header
        uint64_t instanceCount = *reinterpret_cast<uint64_t*>(gameBase + 0x51450 + (playerSlot * 0x40));
        LoadoutInstance* instances =
            reinterpret_cast<LoadoutInstance*>(*reinterpret_cast<uint64_t*>(gameBase + 0x51420 + (playerSlot * 0x40)));

        if (instances && instanceCount > 0 && instanceCount < 16) {
          // Build and send protobuf message
          if (self->GetContext().IsValidForOperations()) {
            realtime::Envelope envelope;
            auto* saveLoadout = envelope.mutable_game_server_save_loadout();

            // Set session and player info
            auto sessionState = self->GetContext().GetSessionState();
            saveLoadout->set_lobby_session_id(sessionState.lobbySessionId);

            // Set entrant ID (account ID as string)
            if (entrant) {
              saveLoadout->set_entrant_id(std::to_string(entrant->userId.accountId));
            }

            saveLoadout->set_loadout_slot(static_cast<int32_t>(playerSlot));
            saveLoadout->set_jersey_number(static_cast<int32_t>(jerseyNumber));

            // Add loadout instances (raw slot/item pairs)
            for (uint64_t i = 0; i < instanceCount; i++) {
              LoadoutInstance* inst = &instances[i];
              auto* protoInstance = saveLoadout->add_loadout_instances();
              protoInstance->set_instance_name(inst->instanceName);

              // Add items
              if (inst->itemsArrayPtr && inst->itemCount > 0 && inst->itemCount < 64) {
                LoadoutItem* items = reinterpret_cast<LoadoutItem*>(inst->itemsArrayPtr);
                for (uint64_t j = 0; j < inst->itemCount; j++) {
                  if (!IsValidSymbolId(items[j].slotType)) continue;
                  auto* protoItem = protoInstance->add_items();
                  protoItem->set_slot_type(items[j].slotType);
                  protoItem->set_equipped_item(items[j].equippedItem);
                }
              }
            }

            // Send via protobuf
            if (SendProtobufEnvelope(self, envelope)) {
              Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Sent protobuf to game service");
            }
          } else {
            Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Not in active session");
          }
        } else {
          Log(EchoVR::LogLevel::Warning,
              "[NEVR.GAMESERVER] [SAVE_LOADOUT] No valid instances found (count=%llu, ptr=%p)", instanceCount,
              instances);
        }
      }
    }
  }
}

void OnMsgSaveLoadoutSuccess(GameServerLib*, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  // Loadout save success received
}

void OnMsgSaveLoadoutPartial(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Save loadout partial (size: %llu)", msgSize);
}

void OnMsgCurrentLoadoutRequest(GameServerLib*, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  // Loadout request received
}

void OnMsgCurrentLoadoutResponse(GameServerLib* self, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  if (!msg || msgSize == 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] Empty response");
    return;
  }

  auto slot = ExtractSlotIndex(msg, msgSize);

  if (slot.slot >= MAX_PLAYER_SLOTS || msgSize < MIN_LOADOUT_MSG_SIZE) {
    return;
  }
}

void OnMsgRefreshProfileForUser(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Refresh profile for user (size: %llu)", msgSize);
}

void OnMsgRefreshProfileFromServer(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Refresh profile from server (size: %llu)", msgSize);
}

void OnMsgLobbySendClientLobbySettings(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Lobby client settings (size: %llu)", msgSize);
}

void OnMsgTierRewardMsg(GameServerLib* self, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Tier reward (size: %llu)", msgSize);
  // TODO: Forward to game service if needed
}

void OnMsgTopAwardsMsg(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Top awards (size: %llu)", msgSize);
}

void OnMsgNewUnlocks(GameServerLib* self, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] New unlocks (size: %llu)", msgSize);
  // TODO: Forward to game service if needed
}

void OnMsgReliableStatUpdate(GameServerLib* self, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Stat update (size: %llu)", msgSize);
  // TODO: Forward to game service if needed
}

void OnMsgReliableTeamStatUpdate(GameServerLib* self, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Team stat update (size: %llu)", msgSize);
  // TODO: Forward to game service if needed
}

void OnTcpMsgGameClientMsg1(GameServerLib*, VOID*, EchoVR::TcpPeer, VOID*, VOID*, UINT64 msgSize) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] TCP game client msg 1 (size: %llu)", msgSize);
}

void OnTcpMsgGameClientMsg2(GameServerLib*, VOID*, EchoVR::TcpPeer, VOID*, VOID*, UINT64 msgSize) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] TCP game client msg 2 (size: %llu)", msgSize);
}

void OnTcpMsgGameClientMsg3(GameServerLib*, VOID*, EchoVR::TcpPeer, VOID*, VOID*, UINT64 msgSize) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] TCP game client msg 3 (size: %llu)", msgSize);
}

// --- GameServerLib Implementation ---

GameServerLib::GameServerLib() : context_(std::make_unique<ServerContext>()) {}

GameServerLib::~GameServerLib() = default;

INT64 GameServerLib::UnkFunc0(VOID*, INT64, INT64) { return 1; }

VOID* GameServerLib::Initialize(EchoVR::Lobby* lobby, EchoVR::Broadcaster* broadcaster, VOID*, const CHAR*) {
  context_->Initialize(lobby, broadcaster);

  RegisterBroadcasterCallbacks();
  RegisterTcpCallbacks();

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Initialized game server");

#if _DEBUG
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] EchoVR base address = 0x%p", EchoVR::g_GameBaseAddress);
#endif

  return this;
}

void GameServerLib::RegisterBroadcasterCallbacks() {
  auto& cb = context_->GetCallbackRegistry();

  cb.sessionStart =
      ListenForBroadcasterMessage(this, Sym::LobbySessionStarting, TRUE, reinterpret_cast<VOID*>(OnMsgSessionStarting));
  cb.sessionError =
      ListenForBroadcasterMessage(this, Sym::LobbySessionError, TRUE, reinterpret_cast<VOID*>(OnMsgSessionError));

  cb.saveLoadout = ListenForBroadcasterMessage(this, Sym::SaveLoadoutRequest, TRUE,
                                               reinterpret_cast<VOID*>(OnMsgSaveLoadoutRequest));
  cb.saveLoadoutSuccess = ListenForBroadcasterMessage(this, Sym::SaveLoadoutSuccess, TRUE,
                                                      reinterpret_cast<VOID*>(OnMsgSaveLoadoutSuccess));
  cb.saveLoadoutPartial = ListenForBroadcasterMessage(this, Sym::SaveLoadoutPartial, TRUE,
                                                      reinterpret_cast<VOID*>(OnMsgSaveLoadoutPartial));
  cb.currentLoadoutRequest = ListenForBroadcasterMessage(this, Sym::CurrentLoadoutRequest, TRUE,
                                                         reinterpret_cast<VOID*>(OnMsgCurrentLoadoutRequest));
  cb.currentLoadoutResponse = ListenForBroadcasterMessage(this, Sym::CurrentLoadoutResponse, TRUE,
                                                          reinterpret_cast<VOID*>(OnMsgCurrentLoadoutResponse));

  cb.refreshProfileForUser = ListenForBroadcasterMessage(this, Sym::RefreshProfileForUser, TRUE,
                                                         reinterpret_cast<VOID*>(OnMsgRefreshProfileForUser));
  cb.refreshProfileFromServer = ListenForBroadcasterMessage(this, Sym::RefreshProfileFromServer, TRUE,
                                                            reinterpret_cast<VOID*>(OnMsgRefreshProfileFromServer));
  cb.lobbySendClientSettings = ListenForBroadcasterMessage(this, Sym::LobbySendClientLobbySettings, TRUE,
                                                           reinterpret_cast<VOID*>(OnMsgLobbySendClientLobbySettings));

  cb.tierReward =
      ListenForBroadcasterMessage(this, Sym::TierRewardMsg, TRUE, reinterpret_cast<VOID*>(OnMsgTierRewardMsg));
  cb.topAwards = ListenForBroadcasterMessage(this, Sym::TopAwardsMsg, TRUE, reinterpret_cast<VOID*>(OnMsgTopAwardsMsg));
  cb.newUnlocks = ListenForBroadcasterMessage(this, Sym::NewUnlocks, TRUE, reinterpret_cast<VOID*>(OnMsgNewUnlocks));

  cb.reliableStatUpdate = ListenForBroadcasterMessage(this, Sym::ReliableStatUpdate, TRUE,
                                                      reinterpret_cast<VOID*>(OnMsgReliableStatUpdate));
  cb.reliableTeamStatUpdate = ListenForBroadcasterMessage(this, Sym::ReliableTeamStatUpdate, TRUE,
                                                          reinterpret_cast<VOID*>(OnMsgReliableTeamStatUpdate));
}

void GameServerLib::RegisterTcpCallbacks() {
  auto& cb = context_->GetCallbackRegistry();

  // Protobuf message handler - all messages from Nakama use protobuf format
  cb.tcpProtobuf = ListenForTcpBroadcasterMessage(this, SYM_PROTOBUF_MSG, reinterpret_cast<VOID*>(OnTcpMsgProtobuf));
}

void GameServerLib::UnregisterAllCallbacks() {
  auto* lobby = context_->GetLobby();
  if (!lobby) return;

  auto& cb = context_->GetCallbackRegistry();

  // Unregister broadcaster callbacks
  if (lobby->broadcaster) {
    EchoVR::BroadcasterUnlisten(lobby->broadcaster, cb.sessionStart);
    EchoVR::BroadcasterUnlisten(lobby->broadcaster, cb.sessionError);
  }

  // Unregister TCP callbacks
  if (lobby->tcpBroadcaster) {
    EchoVR::TcpBroadcasterUnlisten(lobby->tcpBroadcaster, cb.tcpProtobuf);
  }

  cb.Clear();
}

VOID GameServerLib::Terminate() {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Terminated game server");
  context_->Terminate();
}

VOID GameServerLib::Update() {
  // Check for dirty entrants (profile updates pending)
  uint64_t count = context_->GetEntrantCount();
  for (uint64_t i = 0; i < count; ++i) {
    auto* entrant = context_->GetEntrant(static_cast<uint32_t>(i));
    if (entrant && entrant->userId.accountId != 0 && entrant->dirty) {
      // TODO: Handle dirty entrants
    }
  }
}

VOID GameServerLib::UnkFunc1(UINT64) {
  // Called prior to Initialize, purpose unknown
}

VOID GameServerLib::RequestRegistration(INT64 serverId, CHAR*, EchoVR::SymbolId regionId, EchoVR::SymbolId versionLock,
                                        const EchoVR::Json* localConfig) {
  // Update session state
  SessionState state = context_->GetSessionState();
  state.serverId = serverId;
  state.regionId = regionId;
  state.versionLock = versionLock;
  context_->UpdateSessionState(state);

  // Get serverdb URI from config
  CHAR* serverDbUri =
      EchoVR::JsonValueAsString(const_cast<EchoVR::Json*>(localConfig), const_cast<CHAR*>("serverdb_host"),
                                const_cast<CHAR*>("ws://g.echovrce.com:80"), false);

  EchoVR::UriContainer uriContainer = {};
  if (EchoVR::UriContainerParse(&uriContainer, serverDbUri) != ERROR_SUCCESS) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to parse serverdb URI");
    return;
  }

  // Connect to serverdb
  auto* tcp = context_->GetTcpBroadcaster();
  if (!tcp) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] TCP broadcaster unavailable");
    return;
  }

  EchoVR::TcpPeer peer;
  tcp->CreatePeer(&peer, &uriContainer);
  context_->SetServerDbPeer(peer);

  // Build registration request
  auto* broadcaster = context_->GetBroadcaster();
  if (!broadcaster || !broadcaster->data) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Broadcaster unavailable");
    return;
  }

  sockaddr_in gameServerAddr = *reinterpret_cast<sockaddr_in*>(&broadcaster->data->addr);

  // Build protobuf registration request
  realtime::Envelope envelope;
  auto* registration = envelope.mutable_game_server_registration();
  registration->set_login_session_id(GuidToUuidString(state.loginSessionId));
  registration->set_server_id(static_cast<uint64_t>(serverId));
  registration->set_internal_ip_address(Ipv4ToString(gameServerAddr.sin_addr.S_un.S_addr));
  registration->set_port(static_cast<uint32_t>(broadcaster->data->broadcastSocketInfo.port));
  registration->set_region(regionId);
  registration->set_version_lock(versionLock);
  registration->set_time_step_usecs(state.defaultTimeStepUsecs);
#ifdef NEVR_VERSION
  registration->set_version(NEVR_VERSION);
#else
  registration->set_version("unknown");
#endif

  SendProtobufEnvelope(this, envelope);

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Requested game server registration via protobuf");
}

VOID GameServerLib::Unregister() {
  UnregisterAllCallbacks();

  // Disconnect from serverdb
  auto* tcp = context_->GetTcpBroadcaster();
  if (tcp) {
    tcp->DestroyPeer(context_->GetServerDbPeer());
  }

  context_->SetRegistered(false);
  context_->EndSession();

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Unregistered game server");
}

VOID GameServerLib::EndSession() {
  if (context_->IsSessionActive()) {
    realtime::Envelope envelope;
    auto* event = envelope.mutable_lobby_session_event();
    event->set_lobby_session_id(context_->GetSessionState().lobbySessionId);
    event->set_code(realtime::LobbySessionEventMessage::CODE_ENDED);
    SendProtobufEnvelope(this, envelope);
  }

  context_->EndSession();
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Signaling end of session");
}

VOID GameServerLib::LockPlayerSessions() {
  if (context_->IsSessionActive()) {
    realtime::Envelope envelope;
    auto* event = envelope.mutable_lobby_session_event();
    event->set_lobby_session_id(context_->GetSessionState().lobbySessionId);
    event->set_code(realtime::LobbySessionEventMessage::CODE_LOCKED);
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Signaling game server locked");
}

VOID GameServerLib::UnlockPlayerSessions() {
  if (context_->IsSessionActive()) {
    realtime::Envelope envelope;
    auto* event = envelope.mutable_lobby_session_event();
    event->set_lobby_session_id(context_->GetSessionState().lobbySessionId);
    event->set_code(realtime::LobbySessionEventMessage::CODE_UNLOCKED);
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Signaling game server unlocked");
}

VOID GameServerLib::AcceptPlayerSessions(EchoVR::Array<GUID>* playerUuids) {
  if (context_->IsSessionActive()) {
    realtime::Envelope envelope;
    auto* connected = envelope.mutable_lobby_entrant_connected();
    connected->set_lobby_session_id(context_->GetSessionState().lobbySessionId);
    for (uint32_t i = 0; i < playerUuids->count; i++) {
      connected->add_entrant_ids(GuidToUuidString(playerUuids->items[i]));
    }
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Accepted %d players", playerUuids->count);
}

VOID GameServerLib::RemovePlayerSession(GUID* playerUuid) {
  if (context_->IsSessionActive()) {
    realtime::Envelope envelope;
    auto* removed = envelope.mutable_lobby_entrant_removed();
    removed->set_lobby_session_id(context_->GetSessionState().lobbySessionId);
    removed->set_entrant_id(GuidToUuidString(*playerUuid));
    removed->set_code(realtime::LobbyEntrantRemovedMessage::CODE_DISCONNECTED);
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Removed player from game server");
}
