#include "runtime/server/gameserver.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "core/auth_token.h"
#include "auth_token_refresh.h"
#include "runtime/server/constants.h"
#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "runtime/server/messages.h"

#include "runtime/lifecycle/config.h"
#include "runtime/compat/ws_bridge.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/hook/patching.h"
#include "nevr_curl.h"
#include "core/pch.h"
#include "runtime/server/upnp.h"
#include "gameservice/v1/gameservice.pb.h"

/// Read UPnP config from gamepatches globals (same DLL now).
static bool ReadUPnPConfig(NevRUPnPConfig& out) {
    out.enabled = g_upnpEnabled;
    out.port    = g_upnpPort;
    memcpy(out.internalIp, g_internalIpOverride, sizeof(out.internalIp));
    memcpy(out.externalIp, g_externalIpOverride, sizeof(out.externalIp));
    return true;
}

static void CallScheduleReturnToLobby() {
    if (g_pGame) EchoVR::NetGameScheduleReturnToLobby(g_pGame);
}

#include "core/logging.h"

using namespace GameServer;

// D1/N78: this file used to define its own ::Log — a SECOND strong definition of
// the same mangled symbol as src/core/logging.cpp, both linked into
// BugSplat64.dll. Confirmed with nm: `T _Z3LogN6EchoVR8LogLevelEPKcz` in both
// gamepatches.dir/gameserver/gameserver.cpp.obj and common.dir/logging.cpp.obj.
//
// That is an ODR violation, and the two were NOT equivalent: this copy called
// EchoVR::WriteLog unconditionally, while common/logging.cpp null-checks it and
// falls back to stderr. Which one every Log() call in the DLL bound to was
// link-order dependent — and if this one won, every early-boot log line was a
// null function-pointer call and the stderr fallback silently did not exist.
//
// Deleted. common/logging.h declares the guarded one; this file already
// includes it.

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

// Legacy symbol IDs sent by Nakama for backwards compatibility (skipped, handled via protobuf)
constexpr EchoVR::SymbolId SYM_LEGACY_SESSION_START = 0x7777777777770000ULL;
constexpr EchoVR::SymbolId SYM_LEGACY_PLAYERS_REJECTED = 0x7777777777770700ULL;

// Send a protobuf Envelope to ServerDB as binary
bool SendProtobufEnvelope(GameServerLib* self, const gameservice::v1::Envelope& envelope) {
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
    case gameservice::v1::Envelope::kGameServerRegistration:
      msgType = "GameServerRegistration";
      break;
    case gameservice::v1::Envelope::kLobbySessionEvent:
      msgType = "LobbySessionEvent";
      break;
    case gameservice::v1::Envelope::kLobbyEntrantConnected:
      msgType = "LobbyEntrantConnected";
      break;
    case gameservice::v1::Envelope::kLobbyEntrantRemoved:
      msgType = "LobbyEntrantRemoved";
      break;
    case gameservice::v1::Envelope::kGameServerSaveLoadout:
      msgType = "GameServerSaveLoadout";
      break;
    default:
      break;
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Sending protobuf: %s (%zu bytes)", msgType, binaryData.size());

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

  // N102: a dedicated server that ServerDB refused registration to cannot host
  // anything — it would sit idle for hours with nobody watching. Fail fast.
  // ServerFatal (not FatalError) because it is mode-gated: in client mode this
  // is a Warning and execution continues.
  ServerFatal("GameServer registration rejected by ServerDB");
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
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Received session success (SNSLobbySessionSuccessv5), size=%llu",
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
  gameservice::v1::Envelope envelope;
  if (!envelope.ParseFromArray(msg, static_cast<int>(msgSize))) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to parse protobuf Envelope");
    return;
  }

  auto* broadcaster = self->GetContext().GetBroadcaster();

  // Dispatch based on message type
  switch (envelope.message_case()) {
    case gameservice::v1::Envelope::kGameServerRegistrationSuccess: {
      const auto& regSuccess = envelope.game_server_registration_success();
      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Received registration success via protobuf: server_id=%llu, ip=%s",
          static_cast<unsigned long long>(regSuccess.server_id()), regSuccess.external_ip_address().c_str());
      self->GetContext().SetRegistered(true);

      // Encode protobuf to binary format and forward to game
      if (broadcaster) {
        auto encoded = EncodeRegistrationSuccess(regSuccess);
        if (encoded.size() > 0) {
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyRegistrationSuccess,
                                               "SNSLobbyRegistrationSuccess", const_cast<uint8_t*>(encoded.ptr()),
                                               encoded.size());
        } else {
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Failed to encode registration success");
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyRegistrationSuccess,
                                               "SNSLobbyRegistrationSuccess", nullptr, 0);
        }
      }
      break;
    }

    case gameservice::v1::Envelope::kLobbySessionCreate: {
      const auto& sessionCreate = envelope.lobby_session_create();
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Received session create via protobuf: session=%s, max=%d, type=%d",
          sessionCreate.lobby_session_id().c_str(), sessionCreate.max_entrants(), sessionCreate.lobby_type());

      // Update session state
      SessionState state = self->GetContext().GetSessionState();
      state.lobbySessionId = sessionCreate.lobby_session_id();
      self->GetContext().UpdateSessionState(state);

      if (!self->GetContext().StartSession()) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.GAMESERVER] Failed to start session (state=%d, expected Registered=%d)",
            static_cast<int>(self->GetContext().GetState()), static_cast<int>(ServerState::Registered));
      }

      // Start telemetry streaming for this session
      if (g_telemetryEnabled) {
        // Access telemetry via the GameServerLib* — we need to add an accessor
        // This is called via OnTcpMsgProtobuf which has `self` as GameServerLib*
        if (self->GetTelemetry().IsConnected()) {
          self->GetTelemetry().Start(
              sessionCreate.lobby_session_id(),
              g_telemetryRateHz,
              sessionCreate.lobby_type() == 1);  // LOBBY_TYPE_PRIVATE
        }
      }

      // Don't forward protobuf session create to the game — the legacy
      // GameServerSessionStart message carries entrants and resolved level
      // that the protobuf lacks. See the SYM_LEGACY_SESSION_START handler
      // in RegisterTcpCallbacks for the passthrough hack.
      break;
    }

    case gameservice::v1::Envelope::kLobbySessionEvent: {
      const auto& event = envelope.lobby_session_event();
      if (event.code() == gameservice::v1::LobbySessionEventMessage::CODE_ENDED) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.GAMESERVER] Received CODE_ENDED from ServerDB — scheduling return to lobby");
        CallScheduleReturnToLobby();
        self->GetContext().EndSession();
      }
      break;
    }

    case gameservice::v1::Envelope::kLobbySessionSuccessV5: {
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

    case gameservice::v1::Envelope::kLobbyEntrantsAccept: {
      const auto& accept = envelope.lobby_entrants_accept();
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Received entrants accept via protobuf: count=%d",
          accept.entrant_ids_size());

      // Encode protobuf to binary format (padding byte + GUIDs) and forward to game
      if (broadcaster) {
        auto encoded = EncodeLobbyEntrantsAccept(accept);
        if (encoded.size() > 0) {
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyAcceptPlayersSuccessV2,
                                               "SNSLobbyAcceptPlayersSuccessv2", const_cast<uint8_t*>(encoded.ptr()),
                                               encoded.size());
        } else {
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Failed to encode entrants accept");
        }
      }
      break;
    }

    case gameservice::v1::Envelope::kLobbyEntrantReject: {
      const auto& reject = envelope.lobby_entrant_reject();
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Received entrants reject via protobuf: count=%d, code=%d",
          reject.entrant_ids_size(), reject.code());

      // Encode protobuf to binary format (error code byte + GUIDs) and forward to game
      if (broadcaster) {
        auto encoded = EncodeLobbyEntrantsReject(reject);
        if (encoded.size() > 0) {
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyAcceptPlayersFailureV2,
                                               "SNSLobbyAcceptPlayersFailurev2", const_cast<uint8_t*>(encoded.ptr()),
                                               encoded.size());
        } else {
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Failed to encode entrants reject");
        }
      }
      break;
    }

    case gameservice::v1::Envelope::kLobbySmiteEntrant: {
      const auto& smite = envelope.lobby_smite_entrant();
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Received smite entrant via protobuf: entrant=%s, session=%s",
          smite.entrant_id().c_str(), smite.lobby_session_id().c_str());

      // Resolve entrant UUID to slot index. The game engine's SmiteEntrant
      // handler uses the player_id as a slot index for entrant removal.
      GUID entrantGuid = {};
      if (!ParseUuidToGuid(smite.entrant_id(), entrantGuid)) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Invalid entrant UUID: %s", smite.entrant_id().c_str());
        break;
      }

      // Find the entrant's slot index by matching playerSession GUID
      uint64_t slotIndex = 0;
      bool found = false;
      uint64_t entrantCount = self->GetContext().GetEntrantCount();
      for (uint64_t i = 0; i < entrantCount; i++) {
        auto* entrant = self->GetContext().GetEntrant(static_cast<uint32_t>(i));
        if (entrant && memcmp(&entrant->userId, &entrantGuid, sizeof(GUID)) == 0) {
          slotIndex = i;
          found = true;
          break;
        }
      }

      if (!found) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Smite entrant not found in lobby: %s",
            smite.entrant_id().c_str());
        break;
      }

      if (broadcaster) {
        auto encoded = EncodeLobbySmiteEntrant(slotIndex);
        EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbySmiteEntrant, "SNSLobbySmiteEntrant",
                                             const_cast<uint8_t*>(encoded.ptr()), encoded.size());
      }
      break;
    }

    case gameservice::v1::Envelope::kError: {
      const auto& error = envelope.error();
      Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Received error via protobuf: code=%d, msg=%s", error.code(),
          error.message().c_str());
      // If we receive an error before registration succeeds, treat it as a registration failure.
      if (g_exitOnError && !self->GetContext().IsRegistered()) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Error received before registration — shutting down");
        self->BeginGracefulShutdown(true);
      }
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

// LoadoutInstance structure at CR15NetGame + 0x51420 + (slot * 0x40)
// Validated against echovr-reconstruction LoadoutResolver.h:17-24
// (LoadoutInstanceEntry: sizeof == 0x40, offsetof(data) == 0x30, offsetof(loadout_id) == 0x38)
struct LoadoutInstanceHeader {
  uint64_t* instancesPtr;  // +0x00: Pointer to array of LoadoutInstance
  uint64_t _pad08;         // +0x08
  uint64_t _pad10;         // +0x10
  uint64_t _pad18;         // +0x18
  uint64_t _pad20;         // +0x20
  uint64_t _pad28;         // +0x28
  uint64_t instanceCount;  // +0x30: Number of loadout instances (reconstruction: "data")
  uint16_t loadoutNumber;  // +0x38: (reconstruction: "loadout_id")
  uint16_t validation;     // +0x3A
  uint32_t flags;          // +0x3C
};
static_assert(sizeof(LoadoutInstanceHeader) == 0x40, "LoadoutInstanceHeader size mismatch with reconstruction");

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
static_assert(sizeof(LoadoutInstance) == 0x40, "LoadoutInstance size mismatch with reconstruction");

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

  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Slot=%u, GenId=%u, PayloadSize=%llu", slot.slot,
      slot.genId, msgSize);

  if (slot.slot >= MAX_PLAYER_SLOTS) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Invalid slot index: %u", slot.slot);
    return;
  }

  // Log player info
  auto* entrant = self->GetContext().GetEntrant(slot.slot);
  if (entrant) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Player: %s (%s)", entrant->displayName,
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

  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Base=%p, Context=%p", baseAddr, gameContext);

  if (gameContext) {
    // Get CR15NetGame from context + 0x8518
    CHAR* contextBase = reinterpret_cast<CHAR*>(gameContext);
    VOID* netGame = *reinterpret_cast<VOID**>(contextBase + NETGAME_OFFSET);

    Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_LOADOUT] NetGame=%p (from context+0x%X)", netGame,
        NETGAME_OFFSET);

    if (netGame) {
      CHAR* gameBase = reinterpret_cast<CHAR*>(netGame);
      uint16_t playerSlot = slot.slot;

      if (playerSlot < 16) {
        // Read jersey number from gameBase + 0x51458 + (playerSlot * 0x40)
        uint16_t jerseyNumber = *reinterpret_cast<uint16_t*>(gameBase + 0x51458 + (playerSlot * 0x40));
        Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Jersey number: %u", jerseyNumber);

        CHAR* headerAddr = gameBase + 0x51420 + (playerSlot * 0x40);
        (void)headerAddr;

        uint64_t instanceCount = *reinterpret_cast<uint64_t*>(gameBase + 0x51450 + (playerSlot * 0x40));
        LoadoutInstance* instances =
            reinterpret_cast<LoadoutInstance*>(*reinterpret_cast<uint64_t*>(gameBase + 0x51420 + (playerSlot * 0x40)));

        Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Slot %u: %llu loadout instances @ %p", playerSlot,
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
            gameservice::v1::Envelope envelope;
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
              Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Sent protobuf to game service");
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
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_SUCCESS] size=%llu", msgSize);

  if (msg && msgSize > 4) {
    // First 4 bytes are slot info, rest is serialized loadout
    uint8_t* data = reinterpret_cast<uint8_t*>(msg);
    uint32_t slotInfo = *reinterpret_cast<uint32_t*>(data);
    uint16_t slot = slotInfo & 0xFFFF;
    uint16_t genId = (slotInfo >> 16) & 0xFFFF;

    Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_SUCCESS] Slot=%u, GenId=%u, PayloadSize=%llu", slot, genId,
        msgSize - 4);

    // Dump payload (skip 4-byte header)
    size_t dumpLen = (msgSize - 4 > 256) ? 256 : (msgSize - 4);
    char hexBuf[800] = {0};
    int pos = 0;
    for (size_t i = 0; i < dumpLen && pos < 780; i++) {
      pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", data[4 + i]);
      if ((i + 1) % 32 == 0) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_SUCCESS] %s", hexBuf);
        pos = 0;
        hexBuf[0] = 0;
      }
    }
    if (pos > 0) {
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_SUCCESS] %s", hexBuf);
    }
  }
}

void OnMsgSaveLoadoutPartial(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Save loadout partial (size: %llu)", msgSize);
}

void OnMsgCurrentLoadoutRequest(GameServerLib*, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] CurrentLoadoutRequest: size=%llu", msgSize);

  if (msg && msgSize >= sizeof(uint32_t)) {
    uint32_t slotNumber = 0;
    std::memcpy(&slotNumber, msg, sizeof(uint32_t));
    Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Request for slot: %u", slotNumber);
  }
}

void OnMsgCurrentLoadoutResponse(GameServerLib* self, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  if (!msg || msgSize == 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] Empty response");
    return;
  }

  auto slot = ExtractSlotIndex(msg, msgSize);

  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] Response: Slot=%u, GenId=%u, Size=%llu", slot.slot,
      slot.genId, msgSize);

  if (slot.slot >= MAX_PLAYER_SLOTS || msgSize < MIN_LOADOUT_MSG_SIZE) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] Invalid: slot=%u, size=%llu", slot.slot,
        msgSize);
    return;
  }

  auto* entrant = self->GetContext().GetEntrant(slot.slot);
  if (entrant) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] Player: %s (%s)", entrant->displayName,
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
        Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] +%03zu: %s", i - 31, hexBuf);
        pos = 0;
        hexBuf[0] = 0;
      }
    }
    if (pos > 0) {
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] +%03zu: %s", (dumpLen / 32) * 32, hexBuf);
    }
  }
}

void OnMsgRefreshProfileForUser(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Refresh profile for user (size: %llu)", msgSize);
}

void OnMsgRefreshProfileFromServer(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Refresh profile from server (size: %llu)", msgSize);
}

void OnMsgLobbySendClientLobbySettings(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Lobby client settings (size: %llu)", msgSize);
}

void OnMsgTierRewardMsg(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Tier reward (size: %llu)", msgSize);
}

void OnMsgTopAwardsMsg(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Top awards (size: %llu)", msgSize);
}

void OnMsgNewUnlocks(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] New unlocks (size: %llu)", msgSize);
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
    : m_context(std::make_unique<ServerContext>()),
      m_wsClient(std::make_unique<WebSocketClient>()),
      m_telemetry(std::make_unique<TelemetryStreamer>()) {}

GameServerLib::~GameServerLib() {
  // Join the shutdown thread so it finishes before members are destroyed.
  // The thread calls ForceFatalExit (TerminateProcess) at completion, so in the
  // normal graceful-shutdown path the join is interrupted by process death and
  // this destructor never finishes — which is correct.  When the destructor runs
  // without a prior BeginGracefulShutdown call the thread is not joinable and
  // the join is a no-op.
  if (m_shutdownThread.joinable()) {
    m_shutdownThread.join();
  }
}

INT64 GameServerLib::UnkFunc0(VOID* a1, INT64 a2, INT64 a3) {
  fprintf(stderr, "[NEVR.GAMESERVER] UnkFunc0(this=%p, a1=%p, a2=0x%llx, a3=0x%llx)\n",
          (void*)this, a1, (unsigned long long)a2, (unsigned long long)a3);
  fflush(stderr);
  return 1;
}

VOID GameServerLib::UnkFunc1(UINT64 unk) {
  fprintf(stderr, "[NEVR.GAMESERVER] UnkFunc1(this=%p, unk=0x%llx)\n",
          (void*)this, (unsigned long long)unk);
  fflush(stderr);
}

VOID* GameServerLib::Initialize(EchoVR::Lobby* lobby, EchoVR::Broadcaster* broadcaster, VOID* unk2, const CHAR* logPath) {
  fprintf(stderr, "[NEVR.GAMESERVER] Initialize(this=%p, lobby=%p, bc=%p, unk2=%p, logPath=%s)\n",
          (void*)this, (void*)lobby, (void*)broadcaster, unk2, logPath ? logPath : "(null)");
  fflush(stderr);

  m_context->Initialize(lobby, broadcaster);
  m_context->FinalizeInitialization();

  RegisterBroadcasterCallbacks();
  RegisterTcpCallbacks();

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Initialized game server");

  // N87: the game has installed its own console ctrl handler by now, which sits
  // in front of the one InstallConsoleCtrlHandler() registered during
  // Initialize() and swallows CTRL+C by returning TRUE. Move ours back to the
  // front so the shutdown is reported and bounded; ours returns FALSE so the
  // game's teardown (lobby unregistration + ServerDB close) still runs behind
  // it, and we exit cleanly in Terminate() below.
  RearmConsoleCtrlHandler();

#if _DEBUG
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] EchoVR base address = 0x%p", EchoVR::g_GameBaseAddress);
#endif

  return this;
}

void GameServerLib::RegisterBroadcasterCallbacks() {
  auto& cb = m_context->GetCallbackRegistry();

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
  auto& cb = m_context->GetCallbackRegistry();

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
  m_wsClient->SetMessageHandler([this](EchoVR::SymbolId msgId, const VOID* data, UINT64 size) {
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
    } else if (msgId == SYM_LEGACY_SESSION_START) {
      // HACK: Forward legacy GameServerSessionStart directly to the game broadcaster.
      // The protobuf kLobbySessionCreate doesn't carry the entrant descriptors or
      // resolved level that the game needs to set up peer connections. The legacy
      // message does. Once we move to nevr-server-rs with a proper protobuf protocol,
      // this passthrough goes away — the protobuf should carry everything.
      auto* broadcaster = GetContext().GetBroadcaster();
      if (broadcaster) {
        EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbyStartSessionV4,
                                             "SNSLobbyStartSessionv4",
                                             const_cast<VOID*>(data), size);
        Log(EchoVR::LogLevel::Info,
            "[NEVR.GAMESERVER] Forwarded legacy SessionStart to game (HACK — protobuf lacks entrants/level)");
      }
    } else if (msgId == SYM_LEGACY_PLAYERS_REJECTED) {
      // Skip legacy - handled by protobuf kLobbyEntrantReject
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Skipping legacy players rejected (handled via protobuf)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Unhandled WebSocket msgId: 0x%llX (size: %llu)", msgId, size);
    }
  });

  // Re-register on WebSocket reconnection so nakama knows we're available for new sessions.
  // During level transitions the game engine stops calling Update(), messages pile up,
  // and nakama may time out and close the connection. ixwebsocket auto-reconnects but
  // we must re-register to receive new session assignments.
  m_wsClient->SetConnectionHandler([this](BOOL connected) {
    if (!connected) return;

    // Only re-register if we were previously registered (not on first connect)
    if (!m_context->IsRegistered()) return;

    // End any stale session state from before the disconnect
    m_context->EndSession();

    Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] WebSocket reconnected, re-registering with ServerDB");

    SessionState state = m_context->GetSessionState();

    auto* broadcaster = m_context->GetBroadcaster();
    if (!broadcaster || !broadcaster->data) {
      Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Broadcaster unavailable for re-registration");
      return;
    }

    // Resolve IP the same way as initial registration: prefer UPnP/config external IP,
    // fall back to raw socket address. The "internal_ip_address" protobuf field is a
    // legacy misnomer — ServerDB expects the public-facing IP here.
    std::string internalIp = Ipv4ToString(
        reinterpret_cast<sockaddr_in*>(&broadcaster->data->addr)->sin_addr.S_un.S_addr);
    std::string externalIp;
    uint16_t broadcasterPort = broadcaster->data->broadcastSocketInfo.port;

    NevRUPnPConfig upnpCfg = {};
    if (ReadUPnPConfig(upnpCfg)) {
      if (upnpCfg.internalIp[0] != '\0') internalIp = upnpCfg.internalIp;
      if (upnpCfg.externalIp[0] != '\0') externalIp = upnpCfg.externalIp;
      if (upnpCfg.enabled && upnpCfg.port != 0) broadcasterPort = upnpCfg.port;
    }
    if (externalIp.empty()) externalIp = internalIp;

    gameservice::v1::Envelope envelope;
    auto* registration = envelope.mutable_game_server_registration();
    registration->set_login_session_id(GuidToUuidString(g_loginSessionId));
    registration->set_server_id(static_cast<uint64_t>(state.serverId));
    registration->set_internal_ip_address(externalIp);
    registration->set_port(static_cast<uint32_t>(broadcasterPort));
    registration->set_region(state.regionId);
    registration->set_version_lock(state.versionLock);
    registration->set_time_step_usecs(state.defaultTimeStepUsecs);
#ifdef GIT_DESCRIBE
    registration->set_version(GIT_DESCRIBE);
#else
    registration->set_version("unknown");
#endif

    SendProtobufEnvelope(this, envelope);
  });
}

void GameServerLib::UnregisterAllCallbacks() {
  auto* lobby = m_context->GetLobby();
  if (!lobby) return;

  auto& cb = m_context->GetCallbackRegistry();

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
  m_context->Terminate();

  // N87: on the CTRL+C path this is the last point at which the server-visible
  // work is provably finished — "[NSLOBBY] unregistering", "[WEBSOCKET]
  // Disconnected from ServerDB (code: 1000, Normal closure)" and
  // "[NEVR.GAMESERVER] Unregistered game server" are all logged above this line.
  // Everything the game does after this is client-side teardown (level unload,
  // user destruction) that a terminating dedicated server does not need, and it
  // is where the game faults — ACCESS_VIOLATION on an EXECUTE in freed memory,
  // then exit code 5 through its crash-dump path. Exit cleanly here instead.
  // Gated on the console-shutdown flag so a Terminate on any other path is
  // unaffected.
  if (ConsoleShutdownPending()) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.GAMESERVER] CTRL+C teardown complete (lobby unregistered, ServerDB socket closed) "
        "— exiting cleanly before the game's client-side teardown");
    PerformGracefulShutdown(0);
    // Unreachable — PerformGracefulShutdown calls ForceFatalExit.
  }
}

// File-static state for -exitonerror disconnect detection
static bool s_wasConnectedToServerDb = false;
static bool s_exitPending = false;

VOID GameServerLib::Update() {
  // Dispatch incoming ServerDB messages on the main thread
  if (m_wsClient) m_wsClient->ProcessReceivedMessages();

  // Telemetry: snapshot game state and process responses
  if (m_telemetry && m_telemetry->IsActive()) {
    m_telemetry->SnapshotIfDue();
    m_telemetry->ProcessResponses();
  }

  // Telemetry diagnostics: log snapshot data at 1Hz (no WS needed)
  if (g_telemetryDiag && m_telemetry) {
    m_telemetry->RunDiagnostics();
  }

  // -exitonerror: detect serverdb disconnect and trigger graceful shutdown
  if (g_exitOnError && m_wsClient && !s_exitPending) {
    bool nowConnected = m_wsClient->IsConnected();
    if (s_wasConnectedToServerDb && !nowConnected) {
      s_exitPending = true;
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.GAMESERVER] ServerDB disconnected with -exitonerror -- beginning graceful shutdown");
      BeginGracefulShutdown(false);
    }
    s_wasConnectedToServerDb = nowConnected;
  }

  // Check for dirty entrants (profile updates pending)
  uint64_t count = m_context->GetEntrantCount();
  for (uint64_t i = 0; i < count; ++i) {
    auto* entrant = m_context->GetEntrant(static_cast<uint32_t>(i));
    if (entrant && entrant->userId.accountId != 0 && entrant->dirty) {
      // TODO: Handle dirty entrants
    }
  }
}

// UnkFunc1 moved up near UnkFunc0 for vtable logging

void GameServerLib::BeginGracefulShutdown(bool registrationFailed) {
  // Re-entry guard: if a shutdown thread is already running, don't spawn another.
  if (m_shutdownThread.joinable()) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.GAMESERVER] BeginGracefulShutdown already in progress — ignoring redundant call");
    return;
  }

  // Prevent further reconnection attempts so the next disconnect is final.
  if (m_wsClient) m_wsClient->DisableReconnection();

  if (registrationFailed) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] Registration rejected — shutting down");
  }

  auto* self = this;
  m_shutdownThread = std::thread([self, registrationFailed]() {
    constexpr DWORD kMaxWaitMs   = 20 * 60 * 1000;  // 20 minutes
    constexpr DWORD kGraceMs     = 10 * 1000;        // 10 seconds after round end
    constexpr DWORD kPollMs      = 1000;

    if (!registrationFailed && self->GetContext().IsSessionActive()) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.GAMESERVER] Round active — calling ScheduleReturnToLobby, waiting up to 20 min for it to end");
      CallScheduleReturnToLobby();

      DWORD waited = 0;
      while (self->GetContext().IsSessionActive() && waited < kMaxWaitMs) {
        Sleep(kPollMs);
        waited += kPollMs;
      }

      if (self->GetContext().IsSessionActive()) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Round did not end within 20 min — forcing shutdown");
      } else {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.GAMESERVER] Round ended — waiting %lu ms grace period", kGraceMs);
        Sleep(kGraceMs);
      }
    }

    self->EndSession();
    self->Unregister();

    self->m_shutdownComplete.store(true);

    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Graceful shutdown complete — exiting");

    // N64/N105: release the ws_bridge listening socket before ForceFatalExit,
    // or the wineserver keeps it alive as a zombie LISTEN socket.
    // Was a GetProcAddress into ws_bridge.dll — a module that has not been built
    // since the N92 fold, so the lookup returned null and this step silently did
    // nothing. Direct call now; the bridge is in this DLL.
    StopWebSocketBridgeListener();

    // Route through ForceFatalExit: the crash_recovery ExitProcessHook suppresses
    // raw ExitProcess in server mode (to survive the game's crash reporter), which
    // would otherwise swallow this intentional shutdown and leave the process
    // running on torn-down state until it access-violates. ForceFatalExit bypasses
    // the suppression via the saved kernel32 ExitProcess.
    ForceFatalExit(0);
  });
}

// Authenticate the server with Nakama using the operator's discord_id + password.
// Returns a session access JWT, or empty string on failure.
//
// Uses the non-interactive password RPC (docs/guides/token-auth-migration.md F1):
//   POST {nevr_http_uri}/v2/rpc/account/authenticate/password?http_key=<key>&unwrap
//   body {"discord_id","password"} -> {"token","refresh_token"}
// (nakama server/evr_runtime_rpc.go:1137 AuthenticatePasswordRPC, RequireAuth:false;
//  http_key is the transport credential, not server_key Basic auth.) The access
// token's uid is the operator's discord-linked account, which carries the
// server-host role checked at registration (gg.IsServerHost). Verified live
// 2026-06-29: uid=metis.sprock, access token (no vrs.refresh), TTL ~1h.
static std::string AuthenticateServer(const EchoVR::Json* config) {
    CHAR* httpUri = EchoVR::JsonValueAsString(
        const_cast<EchoVR::Json*>(config), const_cast<CHAR*>("nevr_http_uri"), NULL, false);
    CHAR* httpKey = EchoVR::JsonValueAsString(
        const_cast<EchoVR::Json*>(config), const_cast<CHAR*>("nevr_http_key"), NULL, false);
    CHAR* discordId = EchoVR::JsonValueAsString(
        const_cast<EchoVR::Json*>(config), const_cast<CHAR*>("nevr_discord_id"), NULL, false);
    CHAR* password = EchoVR::JsonValueAsString(
        const_cast<EchoVR::Json*>(config), const_cast<CHAR*>("nevr_password"), NULL, false);

    if (!httpUri || !httpKey || !discordId || !password ||
        httpUri[0] == '\0' || httpKey[0] == '\0' || discordId[0] == '\0' || password[0] == '\0') {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] Missing nevr_http_uri/nevr_http_key/nevr_discord_id/nevr_password — cannot authenticate");
        return "";
    }

    std::string url = std::string(httpUri) +
        "/v2/rpc/account/authenticate/password?http_key=" + httpKey + "&unwrap";
    nlohmann::json body;
    body["discord_id"] = discordId;
    body["password"] = password;

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    std::string postData = body.dump();
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nevr::CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
#ifdef NEVR_INSECURE_SKIP_TLS_VERIFY
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
#endif

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] Server auth failed: %s", curl_easy_strerror(res));
        return "";
    }

    if (http_code != 200) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] Server auth HTTP %ld: %s", http_code,
            response.empty() ? "(empty)" : response.substr(0, 200).c_str());
        return "";
    }

    try {
        auto j = nlohmann::json::parse(response);
        std::string token = j.value("token", "");
        if (token.empty()) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.GAMESERVER] Server auth returned empty token");
        } else {
            Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Server authenticated (token acquired)");
        }
        return token;
    } catch (...) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] Server auth response parse error");
        return "";
    }
}

VOID GameServerLib::RequestRegistration(INT64 serverId, CHAR*, EchoVR::SymbolId regionId, EchoVR::SymbolId versionLock,
                                        const EchoVR::Json* localConfig) {
  // Update session state
  SessionState state = m_context->GetSessionState();
  state.serverId = serverId;
  state.regionId = regionId;
  state.versionLock = versionLock;
  m_context->UpdateSessionState(state);

  // Get serverdb URI from config. If not explicitly set, construct from
  // nevr_socket_uri + nevr_discord_id + nevr_password (the common config pattern).
  CHAR* serverDbUri =
      EchoVR::JsonValueAsString(const_cast<EchoVR::Json*>(localConfig), const_cast<CHAR*>("serverdb_host"),
                                nullptr, false);

  // Acquire a session JWT for the operator's server-host account (token auth, BAC-1).
  // Re-auth each registration: the access token TTL is ~1h (BAC-5).
  std::string wsToken;
  {
    auto auth = LoadCachedAuthToken();
    if (auth.HasValidToken()) {
      wsToken = auth.token;
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Using cached auth token for ServerDB");
    } else if (auth.HasValidRefreshToken()) {
      // N106: exchange the refresh token for an access token.
      //
      // This branch was MISSING, which made the OAuth2 device-flow path
      // unreachable on a dedicated server. 571a41b ("access token in-memory
      // only, 60s lifetime, refresh token on disk") stopped persisting the
      // access token — SaveAuthToken writes only refresh_token — so
      // LoadCachedAuthToken().token is ALWAYS empty and HasValidToken() is
      // ALWAYS false in a fresh process. The consumer here was never updated to
      // match, so every server fell through to password auth while a perfectly
      // valid refresh token sat unused on disk.
      //
      // Nothing else refreshes in server mode either: TokenAuth::Init returns
      // early on is_server, before the background refresh thread starts. This is
      // the only place a server can mint an access token.
      CHAR* httpUri = EchoVR::JsonValueAsString(
          const_cast<EchoVR::Json*>(localConfig), const_cast<CHAR*>("nevr_http_uri"), NULL, false);
      CHAR* httpKey = EchoVR::JsonValueAsString(
          const_cast<EchoVR::Json*>(localConfig), const_cast<CHAR*>("nevr_http_key"), NULL, false);
      if (httpUri && httpKey && httpUri[0] != '\0' && httpKey[0] != '\0' &&
          RefreshAuthToken(auth, httpUri, httpKey)) {
        wsToken = auth.token;
        Log(EchoVR::LogLevel::Info,
            "[NEVR.GAMESERVER] ServerDB auth via refreshed OAuth2 token (device-flow credential)");
      } else {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] Refresh token present but exchange failed — falling back to password auth");
      }
    }

    if (wsToken.empty()) {
      wsToken = AuthenticateServer(localConfig);
      // N102: no token means every ServerDB connection below will be rejected.
      // Continuing produces a server that logs connection failures forever
      // instead of exiting with a cause.
      if (wsToken.empty()) {
        ServerFatal("Server authentication failed — no valid token for ServerDB connection");
      }
    }
  }

  thread_local static CHAR constructedUri[1024];
  if (!serverDbUri || serverDbUri[0] == '\0') {
    CHAR* guilds = EchoVR::JsonValueAsString(const_cast<EchoVR::Json*>(localConfig),
                                              const_cast<CHAR*>("nevr_guilds"), nullptr, false);
    CHAR* regions = EchoVR::JsonValueAsString(const_cast<EchoVR::Json*>(localConfig),
                                               const_cast<CHAR*>("nevr_regions"), nullptr, false);
    // Token auth is opt-in: only when nevr_serverdb_uri (the token route, e.g. /nevr) is
    // configured AND a token was acquired. Otherwise fall back to the legacy url-param
    // path so a deploy that hasn't set nevr_serverdb_uri keeps working (no footgun).
    CHAR* tokenUri = EchoVR::JsonValueAsString(const_cast<EchoVR::Json*>(localConfig),
                                                const_cast<CHAR*>("nevr_serverdb_uri"), nullptr, false);

    if (tokenUri && tokenUri[0] != '\0' && !wsToken.empty()) {
      // Token auth (BAC-2): identity via the Bearer JWT (sent by Connect()); discord_id/
      // password dropped; guilds/regions stay as registration metadata. nevr_serverdb_uri
      // points at the token route that forwards the real Authorization header
      // (docs/guides/token-auth-migration.md).
      int written = snprintf(constructedUri, sizeof(constructedUri), "%s", tokenUri);
      const char* sep = "?";
      if (guilds && guilds[0] != '\0' && written > 0 && written < (int)sizeof(constructedUri)) {
        written += snprintf(constructedUri + written, sizeof(constructedUri) - written, "%sguilds=%s", sep, guilds);
        sep = "&";
      }
      if (regions && regions[0] != '\0' && written > 0 && written < (int)sizeof(constructedUri)) {
        snprintf(constructedUri + written, sizeof(constructedUri) - written, "%sregions=%s", sep, regions);
      }
      serverDbUri = constructedUri;
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Constructed serverdb URI for token auth");
    } else {
      // Legacy url-param auth (no nevr_serverdb_uri configured): connect via
      // nevr_socket_uri with discord_id+password — the pre-token-auth behavior.
      CHAR* socketUri = EchoVR::JsonValueAsString(const_cast<EchoVR::Json*>(localConfig),
                                                   const_cast<CHAR*>("nevr_socket_uri"), nullptr, false);
      CHAR* discordId = EchoVR::JsonValueAsString(const_cast<EchoVR::Json*>(localConfig),
                                                   const_cast<CHAR*>("nevr_discord_id"), nullptr, false);
      CHAR* password = EchoVR::JsonValueAsString(const_cast<EchoVR::Json*>(localConfig),
                                                  const_cast<CHAR*>("nevr_password"), nullptr, false);
      if (socketUri && socketUri[0] != '\0' && discordId && discordId[0] != '\0') {
        int written = 0;
        if (password && password[0] != '\0') {
          written = snprintf(constructedUri, sizeof(constructedUri), "%s?discord_id=%s&password=%s", socketUri, discordId, password);
        } else {
          written = snprintf(constructedUri, sizeof(constructedUri), "%s?discord_id=%s", socketUri, discordId);
        }
        if (guilds && guilds[0] != '\0' && written > 0 && written < (int)sizeof(constructedUri)) {
          written += snprintf(constructedUri + written, sizeof(constructedUri) - written, "&guilds=%s", guilds);
        }
        if (regions && regions[0] != '\0' && written > 0 && written < (int)sizeof(constructedUri)) {
          snprintf(constructedUri + written, sizeof(constructedUri) - written, "&regions=%s", regions);
        }
        serverDbUri = constructedUri;
        Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Constructed serverdb URI from config fields (legacy url-param auth)");
      } else {
        serverDbUri = const_cast<CHAR*>("ws://localhost:777/serverdb");
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] No nevr_serverdb_uri/nevr_socket_uri — using default serverdb URI");
      }
    }
  }

  // Connect with the JWT as Authorization: Bearer; the token route forwards it
  // to Nakama's acceptor, which sets the operator identity (BAC-2/3).
  if (!m_wsClient->Connect(serverDbUri, wsToken)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Failed to initiate WebSocket connection");
    return;
  }

  // Build registration request
  auto* broadcaster = m_context->GetBroadcaster();
  if (!broadcaster || !broadcaster->data) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Broadcaster unavailable");
    return;
  }

  sockaddr_in gameServerAddr = *reinterpret_cast<sockaddr_in*>(&broadcaster->data->addr);
  uint16_t broadcasterPort = broadcaster->data->broadcastSocketInfo.port;

  // Resolve IP addresses and apply UPnP / config overrides
  std::string internalIp = Ipv4ToString(gameServerAddr.sin_addr.S_un.S_addr);
  std::string externalIp;  // empty = let UPnP fill it, or falls back to internalIp

  NevRUPnPConfig upnpCfg = {};
  if (ReadUPnPConfig(upnpCfg)) {
    if (upnpCfg.internalIp[0] != '\0') internalIp = upnpCfg.internalIp;
    if (upnpCfg.externalIp[0] != '\0') externalIp = upnpCfg.externalIp;

    if (upnpCfg.enabled) {
      uint16_t extPort = (upnpCfg.port != 0) ? upnpCfg.port : broadcasterPort;
      if (UPnPHelper::OpenPort(broadcasterPort, extPort, externalIp)) {
        broadcasterPort = extPort;
      } else {
        Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] UPnP port mapping failed — using raw broadcaster port");
      }
    } else {
      // N122. The disabled branch was SILENT, so a server that never attempted a
      // mapping looked identical in the log to one where the attempt was never
      // reached at all. Registration announces this port to ServerDB either way,
      // so whether it was forwarded is exactly the thing an operator needs to know.
      Log(EchoVR::LogLevel::Info,
          "[NEVR.UPNP] disabled — announcing raw broadcaster port %u, no mapping attempted "
          "(set \"upnp\" in config or pass -upnp)", broadcasterPort);
    }
  }

  if (externalIp.empty()) externalIp = internalIp;

  // Build protobuf registration request
  gameservice::v1::Envelope envelope;
  auto* registration = envelope.mutable_game_server_registration();
  registration->set_login_session_id(GuidToUuidString(g_loginSessionId));
  registration->set_server_id(static_cast<uint64_t>(serverId));
  registration->set_internal_ip_address(externalIp);  // public-facing IP
  registration->set_port(static_cast<uint32_t>(broadcasterPort));
  registration->set_region(regionId);
  registration->set_version_lock(versionLock);
  registration->set_time_step_usecs(state.defaultTimeStepUsecs);
#ifdef GIT_DESCRIBE
  registration->set_version(GIT_DESCRIBE);
#else
  registration->set_version("unknown");
#endif

  SendProtobufEnvelope(this, envelope);

  // Connect telemetry streamer if enabled
  if (g_telemetryEnabled && m_telemetry) {
    CHAR* telemetryUri =
        EchoVR::JsonValueAsString(const_cast<EchoVR::Json*>(localConfig), const_cast<CHAR*>("telemetry_uri"),
                                  nullptr, false);
    CHAR* telemetryToken =
        EchoVR::JsonValueAsString(const_cast<EchoVR::Json*>(localConfig), const_cast<CHAR*>("telemetry_token"),
                                  nullptr, false);
    if (telemetryUri && telemetryUri[0] != '\0') {
      std::string token;
      if (telemetryToken && telemetryToken[0] != '\0') {
        token = telemetryToken;
      } else {
        // Fall back to cached auth token when telemetry_token not configured
        token = wsToken;
      }
      m_telemetry->Connect(std::string(telemetryUri), token);
    } else {
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] No telemetry_uri in config, telemetry disabled");
    }
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Requested game server registration via protobuf");
}

VOID GameServerLib::Unregister() {
  // Remove UPnP port mapping if we added one
  UPnPHelper::ClosePort();

  // Disconnect telemetry before unregistering
  if (m_telemetry) {
    m_telemetry->Stop();
    m_telemetry->Disconnect();
  }

  UnregisterAllCallbacks();

  // Disconnect WebSocketClient from serverdb
  if (m_wsClient) m_wsClient->Disconnect();

  m_context->SetRegistered(false);
  m_context->EndSession();

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Unregistered game server");
}

VOID GameServerLib::EndSession() {
  // Stop telemetry before ending session
  if (m_telemetry && m_telemetry->IsActive()) {
    m_telemetry->Stop();
  }

  if (m_context->IsSessionActive()) {
    gameservice::v1::Envelope envelope;
    auto* event = envelope.mutable_lobby_session_event();
    event->set_lobby_session_id(m_context->GetSessionState().lobbySessionId);
    event->set_code(gameservice::v1::LobbySessionEventMessage::CODE_ENDED);
    SendProtobufEnvelope(this, envelope);
  }

  m_context->EndSession();
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Signaling end of session");
}

VOID GameServerLib::LockPlayerSessions() {
  if (m_context->IsSessionActive()) {
    gameservice::v1::Envelope envelope;
    auto* event = envelope.mutable_lobby_session_event();
    event->set_lobby_session_id(m_context->GetSessionState().lobbySessionId);
    event->set_code(gameservice::v1::LobbySessionEventMessage::CODE_LOCKED);
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Signaling game server locked");
}

VOID GameServerLib::UnlockPlayerSessions() {
  if (m_context->IsSessionActive()) {
    gameservice::v1::Envelope envelope;
    auto* event = envelope.mutable_lobby_session_event();
    event->set_lobby_session_id(m_context->GetSessionState().lobbySessionId);
    event->set_code(gameservice::v1::LobbySessionEventMessage::CODE_UNLOCKED);
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Signaling game server unlocked");
}

VOID GameServerLib::AcceptPlayerSessions(EchoVR::Array<GUID>* playerUuids) {
  if (m_context->IsSessionActive()) {
    gameservice::v1::Envelope envelope;
    auto* connected = envelope.mutable_lobby_entrant_connected();
    connected->set_lobby_session_id(m_context->GetSessionState().lobbySessionId);
    for (uint32_t i = 0; i < playerUuids->count; i++) {
      connected->add_entrant_ids(GuidToUuidString(playerUuids->items[i]));
    }
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Accepted %d players", playerUuids->count);
}

VOID GameServerLib::RemovePlayerSession(GUID* playerUuid) {
  if (m_context->IsSessionActive()) {
    gameservice::v1::Envelope envelope;
    auto* removed = envelope.mutable_lobby_entrant_removed();
    removed->set_lobby_session_id(m_context->GetSessionState().lobbySessionId);
    removed->set_entrant_id(GuidToUuidString(*playerUuid));
    removed->set_code(gameservice::v1::LobbyEntrantRemovedMessage::CODE_DISCONNECTED);
    SendProtobufEnvelope(this, envelope);
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Removed player from game server");
}
