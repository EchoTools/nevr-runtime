#include "gameserver.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "constants.h"
#include "echovr.h"
#include "echovrunexported.h"
#include "globals.h"
#include "messages.h"
#include "pch.h"
#include "rtapi/v1/realtime_v1.pb.h"

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
bool SendProtobufEnvelope(GameServerLib* self, const rtapi::v1::Envelope& envelope) {
  auto* wsClient = &self->GetWsClient();

  // Serialize envelope to binary
  std::string binaryData;
  if (!envelope.SerializeToString(&binaryData)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to serialize protobuf to binary");
    return false;
  }

  // Log the message type being sent
  const char* msgType = "unknown";
  switch (envelope.message_case()) {
    case rtapi::v1::Envelope::kGameServerRegistration:
      msgType = "GameServerRegistration";
      break;
    case rtapi::v1::Envelope::kLobbySessionEvent:
      msgType = "LobbySessionEvent";
      break;
    case rtapi::v1::Envelope::kLobbyEntrantConnected:
      msgType = "LobbyEntrantConnected";
      break;
    case rtapi::v1::Envelope::kLobbyEntrantRemoved:
      msgType = "LobbyEntrantRemoved";
      break;
    case rtapi::v1::Envelope::kGameServerSaveLoadout:
      msgType = "GameServerSaveLoadout";
      break;
    default:
      break;
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Sending protobuf: %s (%zu bytes)", msgType, binaryData.size());

  // Send via WebSocketClient with protobuf binary symbol
  wsClient->Send(SYM_PROTOBUF_MSG, binaryData.c_str(), binaryData.size());

  return true;
}

// Helper to convert GUID to UUID string format
static std::string GuidToUuidString(const GUID& guid) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
           guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
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

void OnTcpMsgRegistrationSuccess(GameServerLib* self, VOID*, EchoVR::TcpPeer, VOID* msg, VOID*, UINT64 msgSize) {
  self->GetContext().SetRegistered(true);

  auto* broadcaster = self->GetContext().GetBroadcaster();
  if (broadcaster) {
    EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyRegistrationSuccess, "SNSLobbyRegistrationSuccess", msg,
                                         msgSize);
  }
}

void OnTcpMsgRegistrationFailure(GameServerLib* self, VOID*, EchoVR::TcpPeer, VOID* msg, VOID*, UINT64 msgSize) {
  self->GetContext().SetRegistered(false);

  auto* broadcaster = self->GetContext().GetBroadcaster();
  if (broadcaster) {
    EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyRegistrationFailure, "SNSLobbyRegistrationFailure", msg,
                                         msgSize);
  }
}

void OnTcpMessageStartSession(GameServerLib* self, VOID*, EchoVR::TcpPeer, VOID* msg, VOID*, UINT64 msgSize) {
  self->GetContext().StartSession();

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Starting new session");

  auto* broadcaster = self->GetContext().GetBroadcaster();
  if (broadcaster) {
    EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyStartSessionV4, "SNSLobbyStartSessionv4", msg, msgSize);
  }
}

void OnTcpMsgPlayersAccepted(GameServerLib* self, VOID*, EchoVR::TcpPeer, VOID* msg, VOID*, UINT64 msgSize) {
  auto* broadcaster = self->GetContext().GetBroadcaster();
  if (broadcaster) {
    EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyAcceptPlayersSuccessV2,
                                         "SNSLobbyAcceptPlayersSuccessv2", msg, msgSize);
  }
}

void OnTcpMsgPlayersRejected(GameServerLib* self, VOID*, EchoVR::TcpPeer, VOID* msg, VOID*, UINT64 msgSize) {
  auto* broadcaster = self->GetContext().GetBroadcaster();
  if (broadcaster) {
    EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyAcceptPlayersFailureV2,
                                         "SNSLobbyAcceptPlayersFailurev2", msg, msgSize);
  }
}

void OnTcpMsgSessionSuccessv5(GameServerLib* self, VOID*, EchoVR::TcpPeer, VOID* msg, VOID*, UINT64 msgSize) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Received session success (SNSLobbySessionSuccessv5), size=%llu",
      msgSize);

  auto* broadcaster = self->GetContext().GetBroadcaster();
  if (broadcaster) {
    EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbySessionSuccessV5, "SNSLobbySessionSuccessv5",
                                         static_cast<CHAR*>(msg), msgSize);
  }
}

// Handle incoming protobuf messages from Nakama
void OnTcpMsgProtobuf(GameServerLib* self, VOID*, EchoVR::TcpPeer, VOID* msg, VOID*, UINT64 msgSize) {
  if (!msg || msgSize == 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Received empty protobuf message");
    return;
  }

  // Parse the protobuf Envelope
  rtapi::v1::Envelope envelope;
  if (!envelope.ParseFromArray(msg, static_cast<int>(msgSize))) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to parse protobuf Envelope");
    return;
  }

  auto* broadcaster = self->GetContext().GetBroadcaster();

  // Dispatch based on message type
  switch (envelope.message_case()) {
    case rtapi::v1::Envelope::kGameServerRegistrationSuccess: {
      const auto& regSuccess = envelope.game_server_registration_success();
      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Received registration success via protobuf: server_id=%llu, ip=%s",
          static_cast<unsigned long long>(regSuccess.server_id()), regSuccess.external_ip_address().c_str());
      self->GetContext().SetRegistered(true);

      // Encode protobuf to binary format and forward to game
      if (broadcaster) {
        auto encoded = EncodeRegistrationSuccess(regSuccess);
        if (encoded.size() > 0) {
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyRegistrationSuccess,
                                               "SNSLobbyRegistrationSuccess",
                                               const_cast<uint8_t*>(encoded.ptr()), encoded.size());
        } else {
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Failed to encode registration success");
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyRegistrationSuccess,
                                               "SNSLobbyRegistrationSuccess", nullptr, 0);
        }
      }
      break;
    }

    case rtapi::v1::Envelope::kLobbySessionCreate: {
      const auto& sessionCreate = envelope.lobby_session_create();
      Log(EchoVR::LogLevel::Info,
          "[NEVR.GAMESERVER] Received session create via protobuf: session=%s, max=%d, type=%d",
          sessionCreate.lobby_session_id().c_str(), sessionCreate.max_entrants(), sessionCreate.lobby_type());

      // Update session state
      SessionState state = self->GetContext().GetSessionState();
      state.lobbySessionId = sessionCreate.lobby_session_id();
      self->GetContext().UpdateSessionState(state);
      self->GetContext().StartSession();

      // Encode protobuf to LobbyStartSessionV4 binary format and forward to game
      if (broadcaster) {
        auto encoded = EncodeLobbySessionCreate(sessionCreate);
        if (encoded.size() > 0) {
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyStartSessionV4, "SNSLobbyStartSessionv4",
                                               const_cast<uint8_t*>(encoded.ptr()), encoded.size());
        } else {
          Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to encode LobbySessionCreate to binary");
        }
      }
      break;
    }

    case rtapi::v1::Envelope::kLobbySessionSuccessV5: {
      const auto& sessionSuccess = envelope.lobby_session_success_v5();
      Log(EchoVR::LogLevel::Info,
          "[NEVR.GAMESERVER] Received session success via protobuf: lobby=%s, endpoint=%s, game_mode=0x%llX",
          sessionSuccess.lobby_id().c_str(), sessionSuccess.endpoint().c_str(),
          static_cast<unsigned long long>(sessionSuccess.game_mode()));

      SessionState state = self->GetContext().GetSessionState();
      if (!sessionSuccess.lobby_id().empty()) {
        state.lobbySessionId = sessionSuccess.lobby_id();
      }
      self->GetContext().UpdateSessionState(state);

      // Encode protobuf to binary format and forward to game
      if (broadcaster) {
        auto encoded = EncodeLobbySessionSuccessV5(sessionSuccess);
        if (encoded.size() > 0) {
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbySessionSuccessV5, "SNSLobbySessionSuccessv5",
                                               const_cast<uint8_t*>(encoded.ptr()), encoded.size());
        } else {
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Failed to encode LobbySessionSuccessV5");
        }
      }
      break;
    }

    case rtapi::v1::Envelope::kLobbyEntrantsAccept: {
      const auto& accept = envelope.lobby_entrants_accept();
      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Received entrants accept via protobuf: count=%d",
          accept.entrant_ids_size());

      // Encode protobuf to binary format (padding byte + GUIDs) and forward to game
      if (broadcaster) {
        auto encoded = EncodeLobbyEntrantsAccept(accept);
        if (encoded.size() > 0) {
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyAcceptPlayersSuccessV2,
                                               "SNSLobbyAcceptPlayersSuccessv2",
                                               const_cast<uint8_t*>(encoded.ptr()), encoded.size());
        } else {
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Failed to encode entrants accept");
        }
      }
      break;
    }

    case rtapi::v1::Envelope::kLobbyEntrantReject: {
      const auto& reject = envelope.lobby_entrant_reject();
      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Received entrants reject via protobuf: count=%d, code=%d",
          reject.entrant_ids_size(), reject.code());

      // Encode protobuf to binary format (error code byte + GUIDs) and forward to game
      if (broadcaster) {
        auto encoded = EncodeLobbyEntrantsReject(reject);
        if (encoded.size() > 0) {
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyAcceptPlayersFailureV2,
                                               "SNSLobbyAcceptPlayersFailurev2",
                                               const_cast<uint8_t*>(encoded.ptr()), encoded.size());
        } else {
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Failed to encode entrants reject");
        }
      }
      break;
    }

    case rtapi::v1::Envelope::kError: {
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

void OnMsgSessionStarting(GameServerLib*, VOID*, VOID*, UINT64, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Session starting");
}

void OnMsgSessionError(GameServerLib*, VOID*, VOID*, UINT64, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Session error encountered");
}

// Helper to serialize LoadoutSlot to JSON string
static std::string SerializeLoadoutSlot(const EchoVR::LoadoutSlot* slot) {
  char buf[2048];
  snprintf(buf, sizeof(buf),
           R"({"selectionmode":%lld,"banner":%lld,"booster":%lld,"bracer":%lld,"chassis":%lld,)"
           R"("decal":%lld,"decal_body":%lld,"emissive":%lld,"emote":%lld,"secondemote":%lld,)"
           R"("goal_fx":%lld,"medal":%lld,"pattern":%lld,"pattern_body":%lld,"pip":%lld,)"
           R"("tag":%lld,"tint":%lld,"tint_alignment_a":%lld,"tint_alignment_b":%lld,)"
           R"("tint_body":%lld,"title":%lld})",
           (long long)slot->selectionmode, (long long)slot->banner, (long long)slot->booster, (long long)slot->bracer,
           (long long)slot->chassis, (long long)slot->decal, (long long)slot->decal_body, (long long)slot->emissive,
           (long long)slot->emote, (long long)slot->secondemote, (long long)slot->goal_fx, (long long)slot->medal,
           (long long)slot->pattern, (long long)slot->pattern_body, (long long)slot->pip, (long long)slot->tag,
           (long long)slot->tint, (long long)slot->tint_alignment_a, (long long)slot->tint_alignment_b,
           (long long)slot->tint_body, (long long)slot->title);
  return buf;
}

// Helper to serialize LoadoutEntry to JSON string (currently unused, reserved for future use)
[[maybe_unused]] static std::string SerializeLoadoutEntry(const EchoVR::LoadoutEntry* entry) {
  std::string loadoutJson = SerializeLoadoutSlot(&entry->loadout);
  char buf[2560];
  snprintf(buf, sizeof(buf), R"({"bodytype":%lld,"teamid":%u,"airole":%u,"xf":%lld,"loadout":%s})",
           (long long)entry->bodytype, (unsigned)entry->teamid, (unsigned)entry->airole, (long long)entry->xf,
           loadoutJson.c_str());
  return buf;
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

// Helper to serialize a loadout instance to JSON
static std::string SerializeLoadoutInstanceToJson(const LoadoutInstance* instance) {
  std::string json = "{";

  // Instance name as hex (can be converted via hashes.txt)
  char buf[128];
  snprintf(buf, sizeof(buf), "\"instance_name\":\"0x%016llX\"", (unsigned long long)instance->instanceName);
  json += buf;

  // Serialize items - filter out invalid/garbage entries
  json += ",\"items\":{";
  bool first = true;

  if (instance->itemsArrayPtr && instance->itemCount > 0) {
    LoadoutItem* items = reinterpret_cast<LoadoutItem*>(instance->itemsArrayPtr);
    for (uint64_t i = 0; i < instance->itemCount && i < 32; i++) {
      // Skip invalid items (garbage data at end of array)
      if (!IsValidSymbolId(items[i].slotType)) continue;

      if (!first) json += ",";
      first = false;

      snprintf(buf, sizeof(buf), "\"0x%016llX\":\"0x%016llX\"", (unsigned long long)items[i].slotType,
               (unsigned long long)items[i].equippedItem);
      json += buf;
    }
  }

  json += "}}";
  return json;
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
        Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Jersey number: %u", jerseyNumber);

        CHAR* headerAddr = gameBase + 0x51420 + (playerSlot * 0x40);
        (void)headerAddr;

        uint64_t instanceCount = *reinterpret_cast<uint64_t*>(gameBase + 0x51450 + (playerSlot * 0x40));
        LoadoutInstance* instances =
            reinterpret_cast<LoadoutInstance*>(*reinterpret_cast<uint64_t*>(gameBase + 0x51420 + (playerSlot * 0x40)));

        Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Slot %u: %llu loadout instances @ %p", playerSlot,
            instanceCount, instances);

        if (instances && instanceCount > 0 && instanceCount < 16) {
          // Build JSON with jersey number and loadout instances
          std::string fullJson = "{\"slot\":" + std::to_string(playerSlot);
          fullJson += ",\"number\":" + std::to_string(jerseyNumber);
          fullJson += ",\"loadout_instances\":[";

          for (uint64_t i = 0; i < instanceCount; i++) {
            if (i > 0) fullJson += ",";

            LoadoutInstance* inst = &instances[i];
            Log(EchoVR::LogLevel::Info,
                "[NEVR.GAMESERVER] [SAVE_LOADOUT]   Instance %llu: name=0x%016llX, itemsPtr=%p, itemCount=%llu", i,
                (unsigned long long)inst->instanceName, inst->itemsArrayPtr, inst->itemCount);

            fullJson += SerializeLoadoutInstanceToJson(inst);

            // Also log individual items for debugging (only valid SymbolIds)
            if (inst->itemsArrayPtr && inst->itemCount > 0 && inst->itemCount < 64) {
              LoadoutItem* items = reinterpret_cast<LoadoutItem*>(inst->itemsArrayPtr);
              for (uint64_t j = 0; j < inst->itemCount; j++) {
                if (!IsValidSymbolId(items[j].slotType)) continue;  // Skip garbage
                Log(EchoVR::LogLevel::Info,
                    "[NEVR.GAMESERVER] [SAVE_LOADOUT]     Item %llu: slot=0x%016llX, equipped=0x%016llX", j,
                    (unsigned long long)items[j].slotType, (unsigned long long)items[j].equippedItem);
              }
            }
          }
          fullJson += "]}";

          // Output the full JSON (debug)
          Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_LOADOUT] JSON: %s", fullJson.c_str());

          // Build protobuf message
          if (self->GetContext().IsValidForOperations()) {
            rtapi::v1::Envelope envelope;
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

            // Add loadout instances
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
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_SUCCESS] size=%llu", msgSize);

  if (msg && msgSize > 4) {
    // First 4 bytes are slot info, rest is serialized loadout
    uint8_t* data = reinterpret_cast<uint8_t*>(msg);
    uint32_t slotInfo = *reinterpret_cast<uint32_t*>(data);
    uint16_t slot = slotInfo & 0xFFFF;
    uint16_t genId = (slotInfo >> 16) & 0xFFFF;

    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_SUCCESS] Slot=%u, GenId=%u, PayloadSize=%llu", slot, genId,
        msgSize - 4);

    // Dump payload (skip 4-byte header)
    size_t dumpLen = (msgSize - 4 > 256) ? 256 : (msgSize - 4);
    char hexBuf[800] = {0};
    int pos = 0;
    for (size_t i = 0; i < dumpLen && pos < 780; i++) {
      pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", data[4 + i]);
      if ((i + 1) % 32 == 0) {
        Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_SUCCESS] %s", hexBuf);
        pos = 0;
        hexBuf[0] = 0;
      }
    }
    if (pos > 0) {
      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_SUCCESS] %s", hexBuf);
    }
  }
}

void OnMsgSaveLoadoutPartial(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Save loadout partial (size: %llu)", msgSize);
}

void OnMsgCurrentLoadoutRequest(GameServerLib*, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] CurrentLoadoutRequest: size=%llu", msgSize);

  if (msg && msgSize >= sizeof(uint32_t)) {
    uint32_t slotNumber = 0;
    std::memcpy(&slotNumber, msg, sizeof(uint32_t));
    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Request for slot: %u", slotNumber);
  }
}

void OnMsgCurrentLoadoutResponse(GameServerLib* self, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  if (!msg || msgSize == 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] Empty response");
    return;
  }

  auto slot = ExtractSlotIndex(msg, msgSize);

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] Response: Slot=%u, GenId=%u, Size=%llu", slot.slot,
      slot.genId, msgSize);

  if (slot.slot >= MAX_PLAYER_SLOTS || msgSize < MIN_LOADOUT_MSG_SIZE) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] Invalid: slot=%u, size=%llu", slot.slot,
        msgSize);
    return;
  }

  auto* entrant = self->GetContext().GetEntrant(slot.slot);
  if (entrant) {
    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] Player: %s (%s)", entrant->displayName,
        entrant->uniqueName);
  }

  // Dump the serialized loadout payload (skip 4-byte header)
  if (msgSize > 4) {
    uint8_t* data = reinterpret_cast<uint8_t*>(msg);
    size_t payloadSize = msgSize - 4;

    // Dump first 256 bytes of payload in hex
    size_t dumpLen = (payloadSize > 256) ? 256 : payloadSize;
    char hexBuf[800] = {0};
    int pos = 0;
    for (size_t i = 0; i < dumpLen && pos < 780; i++) {
      pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", data[4 + i]);
      if ((i + 1) % 32 == 0) {
        Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] +%03zu: %s", i - 31, hexBuf);
        pos = 0;
        hexBuf[0] = 0;
      }
    }
    if (pos > 0) {
      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] +%03zu: %s", (dumpLen / 32) * 32, hexBuf);
    }
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

void OnMsgTierRewardMsg(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Tier reward (size: %llu)", msgSize);
}

void OnMsgTopAwardsMsg(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Top awards (size: %llu)", msgSize);
}

void OnMsgNewUnlocks(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] New unlocks (size: %llu)", msgSize);
}

void OnMsgReliableStatUpdate(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Stat update (size: %llu)", msgSize);
}

void OnMsgReliableTeamStatUpdate(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Team stat update (size: %llu)", msgSize);
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

GameServerLib::GameServerLib()
    : context_(std::make_unique<ServerContext>()), wsClient_(std::make_unique<WebSocketClient>()) {}

GameServerLib::~GameServerLib() = default;

INT64 GameServerLib::UnkFunc0(VOID*, INT64, INT64) { return 1; }

VOID* GameServerLib::Initialize(EchoVR::Lobby* lobby, EchoVR::Broadcaster* broadcaster, VOID*, const CHAR*) {
  context_->Initialize(lobby, broadcaster);
  context_->FinalizeInitialization();

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

  // Skip TcpBroadcasterListen vtable calls (MinGW/MSVC ABI incompatibility - crashes).
  // Use dummy handle values so UnregisterAllCallbacks() skips them too.
  cb.tcpRegSuccess = 1;
  cb.tcpRegFailure = 1;
  cb.tcpSessionSuccess = 1;
  cb.tcpProtobuf = 1;

  // Route all incoming ServerDB messages through our WebSocketClient instead.
  // Nakama sends both protobuf (NEVRProtobufMessageV1) and legacy messages for backwards
  // compatibility. We handle protobuf messages which properly encode to binary format.
  // Legacy duplicates (registration success, session success) are skipped to avoid
  // double-processing (the game would see the event twice and could misbehave).
  wsClient_->SetMessageHandler([this](EchoVR::SymbolId msgId, const VOID* data, UINT64 size) {
    if (msgId == SYM_PROTOBUF_MSG) {
      OnTcpMsgProtobuf(this, nullptr, {}, const_cast<VOID*>(data), nullptr, size);
    } else if (msgId == TcpSym::LobbyRegistrationFailure) {
      // Registration failure has no protobuf equivalent, handle legacy
      OnTcpMsgRegistrationFailure(this, nullptr, {}, const_cast<VOID*>(data), nullptr, size);
    } else if (msgId == TcpSym::LobbyRegistrationSuccess) {
      // Skip legacy - handled by protobuf kGameServerRegistrationSuccess
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Skipping legacy registration success (handled via protobuf)");
    } else if (msgId == TcpSym::LobbySessionSuccessV5) {
      // Skip legacy - handled by protobuf kLobbySessionSuccessV5
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Skipping legacy session success (handled via protobuf)");
    } else {
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Unhandled WebSocket msgId: 0x%llX (size: %llu)", msgId, size);
    }
  });
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

  // TCP callbacks are handled by WebSocketClient (not game vtable), nothing to unregister here.
  cb.Clear();
}

VOID GameServerLib::Terminate() {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Terminated game server");
  context_->Terminate();
}

// File-static state for -exitonerror disconnect detection
static bool s_wasConnectedToServerDb = false;
static bool s_exitPending = false;

VOID GameServerLib::Update() {
  // Dispatch incoming ServerDB messages on the main thread
  if (wsClient_) wsClient_->ProcessReceivedMessages();

  // -exitonerror: detect serverdb disconnect and exit (immediately or deferred)
  if (exitOnError && wsClient_ && !s_exitPending) {
    bool nowConnected = wsClient_->IsConnected();
    if (s_wasConnectedToServerDb && !nowConnected) {
      s_exitPending = true;
      if (!context_->IsSessionActive()) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] ServerDB disconnected with -exitonerror and no active round -- exiting");
        exit(1);
      } else {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] ServerDB disconnected with -exitonerror -- round active, will exit at round end + 30s");
        auto* ctx = context_.get();
        std::thread([ctx]() {
          while (ctx->IsSessionActive()) {
            Sleep(1000);
          }
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Round ended -- waiting 30s grace period before exit");
          Sleep(30000);
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Grace period elapsed -- exiting");
          exit(1);
        }).detach();
      }
    }
    s_wasConnectedToServerDb = nowConnected;
  }

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
                                const_cast<CHAR*>("ws://localhost:777/serverdb"), false);

  // Connect to serverdb via WebSocketClient (avoids TcpBroadcasterListen vtable ABI crash)
  if (!wsClient_->Connect(serverDbUri)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to initiate WebSocket connection");
    return;
  }

  // Build registration request
  auto* broadcaster = context_->GetBroadcaster();
  if (!broadcaster || !broadcaster->data) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Broadcaster unavailable");
    return;
  }

  sockaddr_in gameServerAddr = *reinterpret_cast<sockaddr_in*>(&broadcaster->data->addr);

  // Build protobuf registration request
  rtapi::v1::Envelope envelope;
  auto* registration = envelope.mutable_game_server_registration();
  registration->set_login_session_id(GuidToUuidString(state.loginSessionId));
  registration->set_server_id(static_cast<uint64_t>(serverId));
  registration->set_internal_ip_address(Ipv4ToString(gameServerAddr.sin_addr.S_un.S_addr));
  registration->set_port(static_cast<uint32_t>(broadcaster->data->broadcastSocketInfo.port));
  registration->set_region(regionId);
  registration->set_version_lock(versionLock);
  registration->set_time_step_usecs(state.defaultTimeStepUsecs);
#ifdef GIT_DESCRIBE
  registration->set_version(GIT_DESCRIBE);
#else
  registration->set_version("unknown");
#endif

  SendProtobufEnvelope(this, envelope);

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Requested game server registration via protobuf");
}

VOID GameServerLib::Unregister() {
  UnregisterAllCallbacks();

  // Disconnect WebSocketClient from serverdb
  if (wsClient_) wsClient_->Disconnect();

  context_->SetRegistered(false);
  context_->EndSession();

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Unregistered game server");
}

VOID GameServerLib::EndSession() {
  if (context_->IsSessionActive()) {
    rtapi::v1::Envelope envelope;
    auto* event = envelope.mutable_lobby_session_event();
    event->set_lobby_session_id(context_->GetSessionState().lobbySessionId);
    event->set_code(rtapi::v1::LobbySessionEventMessage::CODE_ENDED);
    SendProtobufEnvelope(this, envelope);
  }

  context_->EndSession();
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Signaling end of session");
}

VOID GameServerLib::LockPlayerSessions() {
  if (context_->IsSessionActive()) {
    rtapi::v1::Envelope envelope;
    auto* event = envelope.mutable_lobby_session_event();
    event->set_lobby_session_id(context_->GetSessionState().lobbySessionId);
    event->set_code(rtapi::v1::LobbySessionEventMessage::CODE_LOCKED);
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Signaling game server locked");
}

VOID GameServerLib::UnlockPlayerSessions() {
  if (context_->IsSessionActive()) {
    rtapi::v1::Envelope envelope;
    auto* event = envelope.mutable_lobby_session_event();
    event->set_lobby_session_id(context_->GetSessionState().lobbySessionId);
    event->set_code(rtapi::v1::LobbySessionEventMessage::CODE_UNLOCKED);
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Signaling game server unlocked");
}

VOID GameServerLib::AcceptPlayerSessions(EchoVR::Array<GUID>* playerUuids) {
  if (context_->IsSessionActive()) {
    rtapi::v1::Envelope envelope;
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
    rtapi::v1::Envelope envelope;
    auto* removed = envelope.mutable_lobby_entrant_removed();
    removed->set_lobby_session_id(context_->GetSessionState().lobbySessionId);
    removed->set_entrant_id(GuidToUuidString(*playerUuid));
    removed->set_code(rtapi::v1::LobbyEntrantRemovedMessage::CODE_DISCONNECTED);
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Removed player from game server");
}
