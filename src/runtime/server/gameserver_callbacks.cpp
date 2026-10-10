#include "runtime/server/gameserver.h"
#include "core/hex_dump.h"
#include "core/curl_global.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "core/auth_token.h"
#include "auth_token_refresh.h"
#include "runtime/lifecycle/return_to_lobby.h"
#include "runtime/server/constants.h"
#include "runtime/server/failure_detail.h"
#include "runtime/server/protobuf_transport.h"
#include "runtime/server/registration_envelope.h"
#include "runtime/server/serialized_mint.h"
#include "runtime/server/serverdb_uri.h"
#include "runtime/server/session_success_dispatch.h"
#include "runtime/server/session_unregister.h"
#include "runtime/server/callback_unregistration.h"
#include "runtime/log/security_diagnostics.h"
#include "runtime/log/url_diagnostics.h"
#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "core/login_session.h"
#include "core/build_identity.h"  // N112: NEVR build identity
#include "runtime/server/messages.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/service_config.h"  // NevrCfgGetFlat / NevrCfgGetFlatCsv (N133 S4b: config.yaml reads)
#include "runtime/compat/ws_bridge.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/hook/patching.h"
#include "nevr_curl.h"
#include "core/pch.h"
#include "runtime/server/upnp.h"
#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/gameserver_internal.h"

#include "core/logging.h"

using namespace nevr_game_server;

// --- TCP Broadcaster Callbacks ---

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
  // #35/#243: the rejection payload is one BroadcasterRegistrationFailureCode
  // byte, decoded by failure_detail.h.
  const std::string rejection = nevr_failure_detail::DescribeRegistrationRejection(msg, msgSize);
  ServerFatal("GameServer registration rejected by ServerDB: %s", rejection.c_str());
}

// Handle incoming protobuf messages from Nakama. Reads `msg` only (it is parsed,
// never forwarded), so it takes a const pointer.
void OnTcpMsgProtobuf(GameServerLib* self, VOID*, EchoVR::TcpPeer, const VOID* msg, VOID*, UINT64 msgSize) {
  if (!msg || msgSize == 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] empty protobuf message msg=%p size=%llu", msg,
        static_cast<unsigned long long>(msgSize));
    return;
  }

  // Parse the protobuf Envelope
  gameservice::v1::Envelope envelope;
  if (!envelope.ParseFromArray(msg, static_cast<int>(msgSize))) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] protobuf Envelope parse failed size=%llu",
        static_cast<unsigned long long>(msgSize));
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
                                               "SNSLobbyRegistrationSuccess", encoded.ptr(),
                                               encoded.size());
        } else {
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] failed to encode registration success server_id=%llu",
              static_cast<unsigned long long>(regSuccess.server_id()));
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
            "[NEVR.GAMESERVER] session ended by ServerDB session_id=%s — scheduling return to lobby",
            event.lobby_session_id().c_str());
        CallScheduleReturnToLobby();
        self->GetContext().EndSession();
      }
      break;
    }

    case gameservice::v1::Envelope::kLobbySessionSuccessV5: {
      const auto& sessionSuccess = envelope.lobby_session_success_v5();
      Log(EchoVR::LogLevel::Info,
          "[NEVR.GAMESERVER] Received session success via protobuf: lobby=%s, game_mode=0x%llX",
          sessionSuccess.lobby_id().c_str(),
          static_cast<unsigned long long>(sessionSuccess.game_mode()));

      SessionState state = self->GetContext().GetSessionState();
      const auto dispatch = [broadcaster](EncodedMessage& encoded) {
        if (broadcaster) {
          EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbySessionSuccessV5, "SNSLobbySessionSuccessv5",
                                               encoded.ptr(), encoded.size());
        }
      };
      const auto commitState = [self, &state]() { self->GetContext().UpdateSessionState(state); };
      if (!nevr_game_server::ApplyLobbySessionSuccess(sessionSuccess, state.lobbySessionId, commitState, dispatch)) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Failed to encode LobbySessionSuccessV5");
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
                                               "SNSLobbyAcceptPlayersSuccessv2", encoded.ptr(),
                                               encoded.size());
        } else {
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] failed to encode entrants accept count=%d",
              accept.entrant_ids_size());
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
                                               "SNSLobbyAcceptPlayersFailurev2", encoded.ptr(),
                                               encoded.size());
        } else {
          Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] failed to encode entrants reject count=%d code=%d",
              reject.entrant_ids_size(), reject.code());
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

      // Resolve the entrant's slot the way the game resolves accepts: the index of the lobby's
      // player-session slot whose GUID matches (issue #119; echovr.exe 0x140603e20).
      uint64_t slotIndex = 0;
      uint64_t entrantCount = self->GetContext().GetEntrantCount();
      const bool found = self->GetContext().FindEntrantSlotBySession(entrantGuid, slotIndex);

      if (!found) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Smite entrant not found in lobby: %s entrants=%llu",
            smite.entrant_id().c_str(), static_cast<unsigned long long>(entrantCount));
        break;
      }

      Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Smite entrant resolved: entrant=%s slot=%llu entrants=%llu",
          smite.entrant_id().c_str(), static_cast<unsigned long long>(slotIndex),
          static_cast<unsigned long long>(entrantCount));

      if (broadcaster) {
        auto encoded = EncodeLobbySmiteEntrant(slotIndex);
        EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbySmiteEntrant, "SNSLobbySmiteEntrant",
                                             encoded.ptr(), encoded.size());
      }
      break;
    }

    case gameservice::v1::Envelope::kError: {
      const auto& error = envelope.error();
      Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Received error via protobuf: code=%d message_bytes=%zu",
          error.code(), error.message().size());
      // If we receive an error before registration succeeds, treat it as a registration failure.
      if (g_exitOnError && !self->GetContext().IsRegistered()) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] error received before registration, code=%d message_bytes=%zu — shutting down",
            error.code(), error.message().size());
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

void OnMsgSessionStarting(GameServerLib* self, VOID*, VOID*, UINT64, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] session starting session_id=%s",
      self->GetContext().GetSessionState().lobbySessionId.c_str());
}

void OnMsgSessionError(GameServerLib* self, VOID*, VOID*, UINT64, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] session error session_id=%s",
      self->GetContext().GetSessionState().lobbySessionId.c_str());
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
  nlohmann::json j;

  char buf[32];
  snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)instance->instanceName);
  j["instance_name"] = buf;

  auto& items = j["items"];
  items = nlohmann::json::object();

  if (instance->itemsArrayPtr && instance->itemCount > 0) {
    LoadoutItem* rawItems = reinterpret_cast<LoadoutItem*>(instance->itemsArrayPtr);
    for (uint64_t i = 0; i < instance->itemCount && i < 32; i++) {
      if (!IsValidSymbolId(rawItems[i].slotType)) continue;

      char keyBuf[32], valBuf[32];
      snprintf(keyBuf, sizeof(keyBuf), "0x%016llX", (unsigned long long)rawItems[i].slotType);
      snprintf(valBuf, sizeof(valBuf), "0x%016llX", (unsigned long long)rawItems[i].equippedItem);
      items[keyBuf] = valBuf;
    }
  }

  return j.dump();
}

void OnMsgSaveLoadoutRequest(GameServerLib* self, VOID*, VOID* msg, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  if (!msg || msgSize < MIN_LOADOUT_MSG_SIZE) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Invalid message size: %llu (min=%llu)", msgSize,
        static_cast<unsigned long long>(MIN_LOADOUT_MSG_SIZE));
    return;
  }

  auto slot = ExtractSlotIndex(msg, msgSize);

  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Slot=%u, GenId=%u, PayloadSize=%llu", slot.slot,
      slot.genId, msgSize);

  if (slot.slot >= MAX_PLAYER_SLOTS) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Invalid slot index: %u (max=%u)", slot.slot,
        MAX_PLAYER_SLOTS);
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
              Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Sent protobuf to game service slot=%u",
                  playerSlot);
            }
          } else {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.GAMESERVER] [SAVE_LOADOUT] Not in active session, slot=%u — loadout not sent", playerSlot);
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
    for (const std::string& line : nevr::HexDumpLines(data + 4, msgSize - 4, 256, 32)) {
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [SAVE_SUCCESS] %s", line.c_str());
    }
  }
}

void OnMsgSaveLoadoutPartial(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  // S1: only what's actually known here is that the game's own size threshold
  // for a partial SaveLoadout message was triggered — the protocol semantics
  // of what "partial" means (chunked transfer vs. truncated payload) aren't
  // documented in this file, so don't assert truncation as established fact.
  Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] SaveLoadoutPartial received (size threshold triggered) size=%llu",
      msgSize);
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
    size_t offset = 0;
    for (const std::string& line : nevr::HexDumpLines(data + 4, payloadSize, 256, 32)) {
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] [CURRENT_LOADOUT] +%03zu: %s", offset, line.c_str());
      offset += 32;
    }
  }
}

void OnMsgRefreshProfileForUser(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] refresh profile for user (no server-side action — observability only) size=%llu", msgSize);
}

void OnMsgRefreshProfileFromServer(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] refresh profile from server (no server-side action — observability only) size=%llu", msgSize);
}

void OnMsgLobbySendClientLobbySettings(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] lobby client settings received (no server-side action — observability only) size=%llu", msgSize);
}

void OnMsgTierRewardMsg(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] tier reward received (no server-side action — observability only) size=%llu", msgSize);
}

void OnMsgTopAwardsMsg(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] top awards received (no server-side action — observability only) size=%llu", msgSize);
}

void OnMsgNewUnlocks(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] new unlocks received (no server-side action — observability only) size=%llu", msgSize);
}

void OnMsgReliableStatUpdate(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] stat update received (no server-side action — observability only) size=%llu", msgSize);
}

void OnMsgReliableTeamStatUpdate(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] team stat update received (no server-side action — observability only) size=%llu", msgSize);
}

void GameServerLib::RegisterBroadcasterCallbacks() {
  m_registryThreadId.store(GetCurrentThreadId());
  auto& cb = m_context->GetCallbackRegistry();
  // Issue #117: without the owner, UnregisterAllCallbacks never reaches
  // EchoVR::BroadcasterUnlisten (merge 033b303 dropped this from ba6b5f0).
  EchoVR::Broadcaster* owner = nevr_game_server::RecordBroadcasterOwner(*m_context);
  Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Broadcaster callback owner=%p", static_cast<void*>(owner));

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
  // `data` is a writable, dispatcher-owned copy (WebSocketClient::MessageCallback).
  // It must be: two branches below hand it to CBroadcaster::ReceiveLocalEvent
  // (echovr.exe 0x140F87AA0), which passes the msg pointer on, as mutable, to every
  // listener registered for the symbol (issue #43).
  m_wsClient->SetMessageHandler([this](EchoVR::SymbolId msgId, VOID* data, UINT64 size) {
    if (msgId == SYM_PROTOBUF_MSG) {
      OnTcpMsgProtobuf(this, nullptr, {}, data, nullptr, size);
    } else if (msgId == TcpSym::LobbyRegistrationFailure) {
      // Registration failure has no protobuf equivalent, handle legacy
      OnTcpMsgRegistrationFailure(this, nullptr, {}, data, nullptr, size);
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
                                             "SNSLobbyStartSessionv4", data, size);
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

    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] websocket reconnected — re-registering server_id=%llu",
        static_cast<unsigned long long>(m_context->GetSessionState().serverId));

    SessionState state = m_context->GetSessionState();

    auto* broadcaster = m_context->GetBroadcaster();
    if (!broadcaster || !broadcaster->data) {
      Log(EchoVR::LogLevel::Error,
          "[NEVR.GAMESERVER] broadcaster unavailable — re-registration aborted, will retry on next reconnect");
      return;
    }

    // Resolve IP the same way as initial registration: prefer UPnP/config external IP,
    // fall back to raw socket address. The "internal_ip_address" protobuf field is a
    // legacy misnomer — ServerDB expects the public-facing IP here.
    std::string internalIp = Ipv4ToString(
        reinterpret_cast<sockaddr_in*>(&broadcaster->data->addr)->sin_addr.S_un.S_addr);
    std::string externalIp;
    uint16_t broadcasterPort = broadcaster->data->broadcastSocketInfo.port;

    NevrUPnPConfig upnpCfg = {};
    if (ReadUPnPConfig(upnpCfg)) {
      if (upnpCfg.internalIp[0] != '\0') internalIp = upnpCfg.internalIp;
      if (upnpCfg.externalIp[0] != '\0') externalIp = upnpCfg.externalIp;
      if (upnpCfg.enabled && upnpCfg.port != 0) broadcasterPort = upnpCfg.port;
    }
    if (externalIp.empty()) externalIp = internalIp;

    const BuildIdentity::Info& buildId = BuildIdentity::Get();  // N112: commit hash and build type in the version
    nevr_game_server::RegistrationParams params;
    params.loginSessionId = GuidToUuidString(LoginSession::Get());
    params.serverId = static_cast<uint64_t>(state.serverId);
    params.externalIp = externalIp;
    params.port = static_cast<uint32_t>(broadcasterPort);
    params.regionId = state.regionId;
    params.versionLock = state.versionLock;
    params.timeStepUsecs = state.defaultTimeStepUsecs;
    params.version = nevr_game_server::FormatRegistrationVersion(buildId.git_describe, buildId.git_commit, buildId.build_type);
    const gameservice::v1::Envelope envelope = nevr_game_server::BuildRegistrationEnvelope(params);

    if (!SendProtobufEnvelope(this, envelope)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] protobuf serialize failed for re-registration");
    }
  });
}

void GameServerLib::UnregisterAllCallbacks() {
  // GH #44: the registry and EchoVR::BroadcasterUnlisten are game-thread-only
  // (server_context.h). Make any future off-thread caller visible in the log.
  const DWORD registryThread = m_registryThreadId.load();
  const DWORD thisThread = GetCurrentThreadId();
  if (registryThread != 0 && thisThread != registryThread) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] callback registry reached from thread %lu; callbacks were registered on game thread %lu "
        "(registry is game-thread-only, GH #44)",
        static_cast<unsigned long>(thisThread), static_cast<unsigned long>(registryThread));
  }

  auto* lobby = m_context->GetLobby();
  auto& cb = m_context->GetCallbackRegistry();
  EchoVR::Broadcaster* liveOwner = lobby != nullptr ? lobby->broadcaster : nullptr;
  const nevr_game_server::BroadcasterUnlisten unlisten = EchoVR::BroadcasterUnlisten == nullptr
      ? nevr_game_server::BroadcasterUnlisten{}
      : nevr_game_server::BroadcasterUnlisten([](EchoVR::Broadcaster* owner, uint16_t handle) {
          EchoVR::BroadcasterUnlisten(owner, handle);
        });
  // Issue #122: this is the line that would have caught #117 in production.
  // #117's signature was recordedOwner reading null (never recorded) while
  // liveOwner was a real pointer — the mismatch that makes removed stay 0
  // with no actual error. It was Debug, which isn't on by default, so that
  // week-long silent no-op went unseen.
  const EchoVR::Broadcaster* recordedOwner = cb.broadcasterOwner;
  const size_t removed = nevr_game_server::UnregisterBroadcasterCallbacks(liveOwner, cb, unlisten);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.GAMESERVER] Unregistered %zu broadcaster callbacks (owner=%p, liveOwner=%p)",
      removed, static_cast<const void*>(recordedOwner), static_cast<void*>(liveOwner));
}
