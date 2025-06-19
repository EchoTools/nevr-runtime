#include "gameserver.h"

#include <google/protobuf/util/json_util.h>  // Include for JSON serialization
#include <ws2tcpip.h>                        // Include for inet_pton and other socket functions

#include <cstdio>
#include <string>
#include <vector>

#include "echovr.h"
#include "echovrInternal.h"
#include "generated/rtapi.pb.h"
#include "globals.h"
#include "logging.h"
#include "messages.h"
#include "pch.h"

/// A wrapper for WriteLog, simplifying logging operations.
VOID Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  va_list args;
  va_start(args, format);
  EchoVR::WriteLog(level, 0, format, args);
  va_end(args);
}

// Function to follow a pointer chain
uintptr_t FollowPointerChain(const std::vector<uintptr_t>& offsets) {
  uintptr_t address = 0;
  for (size_t i = 0; i < offsets.size() - 1; ++i) {
    // Log(EchoVR::LogLevel::Debug, "%p+%p -> %p", address, (void *)offsets[i], *reinterpret_cast<uintptr_t *>(address +
    // offsets[i]));
    address = *reinterpret_cast<uintptr_t*>(address + offsets[i]);  // Dereference the address
  }
  address = address + offsets[offsets.size() - 1];
  return address;  // Final address after following the pointer chain
}

UINT32 GetTimeStepUsecs() {
  UINT32* timeStepUsecs = (UINT32*)(*(CHAR**)(EchoVR::g_GameBaseAddress + 0x020A00E8) + 0x90);
  return *timeStepUsecs;  // Return the value at the address
}

UINT32 SetTimeStepUsecs(UINT32 newTimeStepUsecs) {
  UINT32* timeStepUsecs = (UINT32*)(*(CHAR**)(EchoVR::g_GameBaseAddress + 0x020A00E8) + 0x90);
  UINT32 oldValue = *timeStepUsecs;   // Store the old value
  *timeStepUsecs = newTimeStepUsecs;  // Set the new value
  return oldValue;                    // Return the old value
}

// Retrieve the Login UUID from the game's memory
GUID GetLoginUUID() {
  uintptr_t baseAddress = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress) + 0x020B2E28;
  std::vector<uintptr_t> offsets = {baseAddress, 0, 0x978};

  // Follow the pointer chain to get the login UUID
  uintptr_t loginUUIDAddress = FollowPointerChain(offsets);

  // Read the login UUID from the address
  GUID loginUUID = *reinterpret_cast<GUID*>(loginUUIDAddress);

  return loginUUID;
}

// Converts a GUID to a string representation
char* GuidToString(const GUID& guid, char (&buffer)[37]) {
  memset(buffer, 0, 37);
  sprintf_s(buffer, 37, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid.Data1, guid.Data2, guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
            guid.Data4[7]);
  return buffer;
}

// Converts a string representation to a GUID
HRESULT StringToGuid(const std::string& guidString, GUID* outGuid) {
  if (guidString.empty() || outGuid == nullptr) {
    return E_INVALIDARG;
  }

  // Format should be: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
  unsigned long p0;
  unsigned short p1, p2;
  unsigned int p3, p4, p5, p6, p7, p8, p9, p10;

  int result = sscanf_s(guidString.c_str(), "%8lx-%4hx-%4hx-%2x%2x-%2x%2x%2x%2x%2x%2x", &p0, &p1, &p2, &p3, &p4, &p5,
                        &p6, &p7, &p8, &p9, &p10);

  if (result != 11) {
    return E_INVALIDARG;
  }

  outGuid->Data1 = p0;
  outGuid->Data2 = p1;
  outGuid->Data3 = p2;
  outGuid->Data4[0] = (unsigned char)p3;
  outGuid->Data4[1] = (unsigned char)p4;
  outGuid->Data4[2] = (unsigned char)p5;
  outGuid->Data4[3] = (unsigned char)p6;
  outGuid->Data4[4] = (unsigned char)p7;
  outGuid->Data4[5] = (unsigned char)p8;
  outGuid->Data4[6] = (unsigned char)p9;
  outGuid->Data4[7] = (unsigned char)p10;

  return S_OK;
}

// Sends a protobuf message to the server, optionally using JSON serialization
void SendProtobufMessage(GameServerLib* self, const nevr::rtapi::Envelope& protoEnvelope) {
// #define USEJSON 1  // Define this to enable JSON serialization
#ifdef USEJSON
  std::string jsonString;
  google::protobuf::util::JsonPrintOptions jsonOptions;
  jsonOptions.add_whitespace = true;                        // For pretty printing (optional)
  jsonOptions.always_print_fields_with_no_presence = true;  // Print all fields, even if unset
  jsonOptions.preserve_proto_field_names = true;            // Use original .proto field names (snake_case)

  // Serialize the protobuf message to JSON
  auto status = google::protobuf::util::MessageToJsonString(protoEnvelope, &jsonString, jsonOptions);

  if (!status.ok()) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Failed to serialize protobuf message to JSON: %s",
        status.ToString().c_str());
    return;
  }

  Log(EchoVR::LogLevel::Warning, "[NEVR.SERVER] Serialized JSON: %s",
      jsonString.c_str());  // Log the JSON for debugging

  self->tcpBroadcasterData->SendToPeer(self->serverDbPeer, SYMBOL_TCPBROADCASTER_NEVRPROTOBUF_JSON_MESSAGE_V1, NULL, 0,
                                       jsonString.c_str(), jsonString.size());

#else  // If not using JSON, serialize the protobuf message directly
       // Serialize the protobuf message
  std::string serializedMessage;
  if (!protoEnvelope.SerializeToString(&serializedMessage)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Failed to serialize protobuf message");
    return;
  }

  self->tcpBroadcasterData->SendToPeer(self->serverDbPeer, SYMBOL_TCPBROADCASTER_NEVRPROTOBUF_MESSAGE_V1, NULL, 0,
                                       serializedMessage.c_str(), serializedMessage.size());

#endif
}

/// Subscribes to internal local events for a given message type. These are
/// typically sent internally by the game to its self, or derived from connected
/// peer's messages (UDP broadcast port forwards events). The provided function
/// is used as a callback when a message of the type is received from any peer
/// or onesself.
/// <param name="self">The game server library which is listening for the
/// message.</param> <param name="msgId">The 64-bit symbol used to describe a
/// message type/identifier to listen for.</param> <param
/// name="isMsgReliable">Indicates whether we are listening for events for
/// messages sent over the reliable or mailbox game server message inbox
/// types.</param> <param name="func">The function to use as callback when a
/// broadcaster message of the given type is received.</param> <returns>An
/// identifier/handle for the callback registration, to be later used in
/// unregistering.</returns>
UINT16
ListenForBroadcasterMessage(GameServerLib* self, EchoVR::SymbolId msgId, BOOL isMsgReliable, VOID* func) {
  // Subscribe to the provided message id.
  EchoVR::DelegateProxy listenerProxy;
  memset(&listenerProxy, 0, sizeof(listenerProxy));
  listenerProxy.method[0] = 0xFFFFFFFFFFFFFFFF;
  listenerProxy.instance = (VOID*)self;
  listenerProxy.proxyFunc = func;

  return EchoVR::BroadcasterListen(self->lobby->broadcaster, msgId, isMsgReliable, (VOID*)&listenerProxy, true);
}

/// Subscribes the TCP broadcasters (websocket) to a given message type. The
/// provided function is used as a callback when a message of the type is
/// received from any service.
/// <param name="self">The game server library which is listening for the
/// message.</param> <param name="msgId">The 64-bit symbol used to describe a
/// message type/identifier to listen for.</param> <param name="func">The
/// function to use as callback when a TCP/webosocket message of the given type
/// is received.</param> <returns>None</returns>
UINT16 ListenForTcpBroadcasterMessage(GameServerLib* self, EchoVR::SymbolId msgId, VOID* func) {
  // Subscribe to the provided message id.
  // Normally a proxy function is provided, which calls the underlying method
  // provided, but we just use the proxy function callback to receive everything
  // to keep it simple.
  EchoVR::DelegateProxy listenerProxy;
  memset(&listenerProxy, 0, sizeof(listenerProxy));
  listenerProxy.method[0] = 0xFFFFFFFFFFFFFFFF;
  listenerProxy.instance = (VOID*)self;
  listenerProxy.proxyFunc = func;

  return EchoVR::TcpBroadcasterListen(self->lobby->tcpBroadcaster, msgId, 0, 0, 0, (VOID*)&listenerProxy, true);
}

// Send a message to the ServerDB service over TCP (websocket).
VOID SendServerdbTcpMessage(GameServerLib* self, EchoVR::SymbolId msgId, VOID* msg, UINT64 msgSize) {
  // Wrap the send call provided by the TCP broadcaster.
  self->tcpBroadcasterData->SendToPeer(self->serverDbPeer, msgId, NULL, 0, msg, msgSize);
}

// Message from game service that the player has been rejected from the
// session, and the game server should reject them.
VOID OnTcpMsgRemovePlayerSession(GameServerLib* self, VOID* proxymthd, EchoVR::TcpPeer sender, VOID* msg, VOID* unk,
                                 UINT64 msgSize) {
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_ACCEPT_PLAYERS_FAILURE_V2,
                                       "SNSLobbyAcceptPlayersFailurev2", msg, msgSize);
}

// Message from game service with connection parameters for player-server communication
VOID OnTcpMsgPlayerConnectDetails(GameServerLib* self, VOID* proxymthd, EchoVR::TcpPeer sender, VOID* msg, VOID* unk,
                                  UINT64 msgSize) {
  // Forward the received join session success event to the internal broadcast.
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_SESSION_SUCCESS_V5,
                                       "SNSLobbySessionSuccessv5", msg, msgSize);
}

INT64 GameServerLib::UnkFunc0(VOID* unk1, INT64 a2, INT64 a3) { return 1; }

namespace internalPipeline {

/// Event handler for receiving a session start success message from events
/// (internal game server broadcast). This message indicates that a new session
/// is starting.This is triggered after a SNSLobbyStartSessionv4 message is
/// processed.
VOID sessionStarting(GameServerLib* self, VOID* proxymthd, VOID* msg, UINT64 msgSize, EchoVR::Peer destination,
                     EchoVR::Peer sender) {
  // NOTE: `msg` here has no substance (one uninitialized byte).
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Session starting");
}

/// Event handler for receiving a session error message from events (internal
/// game server broadcast). This message indicates that the game session
/// encountered an error either when starting or running.
VOID sessionError(GameServerLib* self, VOID* proxymthd, VOID* msg, UINT64 msgSize, EchoVR::Peer destination,
                  EchoVR::Peer sender) {
  // NOTE: `msg` here has no substance (one uninitialized byte).
  Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Session error encountered");
}

VOID returnToLobby(GameServerLib* self, VOID* proxymthd, VOID* msg, UINT64 msgSize, EchoVR::Peer destination,
                   EchoVR::Peer sender) {
  // NOTE: `msg` here has no substance (one uninitialized byte).
  return;  // Currently broken
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Returning to lobby");
  EchoVR::NetGameScheduleReturnToLobby(self);
}
}  // namespace internalPipeline

namespace Pipeline {

VOID gameServerRegistrationSuccess(GameServerLib* self, const nevr::rtapi::Envelope& envelope) {
  const auto& response = envelope.gameserverregistrationsuccess();
  // Log the successful registration
  self->registered = TRUE;
  // Build the message to send to the internal broadcaster.
  UINT64 serverId = response.server_id();
  // Convert the IP address string to 4-byte representation.
  sockaddr_in serverAddr;
  memset(&serverAddr, 0, sizeof(serverAddr));
  serverAddr.sin_family = AF_INET;
  // Convert the IP address string to a binary format
  if (inet_pton(AF_INET, response.external_ip_address().c_str(), &serverAddr.sin_addr) <= 0) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Invalid IP address format: %s", response.external_ip_address().c_str());
    return;
  }
  // Prepare buffer to hold: server_id (UINT64), IP address (UINT32), and unknown value (UINT64)
  constexpr size_t SERVER_ID_SIZE = sizeof(UINT64);
  constexpr size_t IP_ADDR_SIZE = sizeof(UINT32);
  constexpr size_t UNK_VALUE_SIZE = sizeof(UINT64);
  constexpr size_t bufferSize = SERVER_ID_SIZE + IP_ADDR_SIZE + UNK_VALUE_SIZE;
  auto messageBuffer = new CHAR[bufferSize];
  memset(messageBuffer, 0, bufferSize);
  memcpy(messageBuffer, &serverId, sizeof(UINT64));
  memcpy(messageBuffer + SERVER_ID_SIZE, &serverAddr.sin_addr.s_addr, IP_ADDR_SIZE);
  // Third field is zeroed out (already done by memset)
  // Forward the received event to the internal broadcast.
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_REGISTRATION_SUCCESS,
                                       "SNSLobbyRegistrationSuccess", messageBuffer, bufferSize);
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Game server registration successful");
}

VOID lobbySessionCreate(GameServerLib* self, const nevr::rtapi::Envelope& envelope) {
  const auto& response = envelope.lobbysessioncreate();

  // Construct the message to send to the internal broadcaster.
  GameServerSessionStartInternalMessage sessionStartMessage;
  // Convert the lobby session ID from string to GUID
  if (StringToGuid(response.lobby_session_id(), &sessionStartMessage.LobbySesssionId) != ERROR_SUCCESS) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Failed to convert match ID to GUID: %s",
        response.lobby_session_id().c_str());
    return;
  }
  // Convert the group ID from string to GUID
  if (StringToGuid(response.group_id(), &sessionStartMessage.GroupId) != ERROR_SUCCESS) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Failed to convert group ID to GUID: %s", response.group_id().c_str());
    return;
  }
  // Set the player limit and entrant count
  sessionStartMessage.PlayerLimit = response.max_entrants();
  sessionStartMessage.EntrantCount = 0;
  sessionStartMessage.LobbyType = response.lobby_type();
  sessionStartMessage.Pad1 = 0;  // Padding byte
  sessionStartMessage.SettingsJson = new CHAR[response.settings_json().size() + 1];
  // Copy the settings JSON into the message
  memset(sessionStartMessage.SettingsJson, 0, response.settings_json().size() + 1);
  memcpy(sessionStartMessage.SettingsJson, response.settings_json().c_str(), response.settings_json().size());

  self->sessionActive = TRUE;

  // message buffer to send to the internal broadcaster.
  auto messageBuffer = new CHAR[sizeof(GameServerSessionStartInternalMessage)];
  memset(messageBuffer, 0, sizeof(GameServerSessionStartInternalMessage));
  // Copy the session start message into the buffer
  memcpy(messageBuffer, &sessionStartMessage, sizeof(GameServerSessionStartInternalMessage));
  // Forward the received event to the internal broadcast.
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Creating/starting new session");
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_START_SESSION_V4,
                                       "SNSLobbyStartSessionv4", messageBuffer,
                                       sizeof(GameServerSessionStartInternalMessage));
}

// Event handler for receiving a game server entrant join.
VOID lobbyEntrantsAccept(GameServerLib* self, const nevr::rtapi::Envelope& envelope) {
  const auto& response = envelope.lobbyentrantsaccept();
  if (response.entrant_ids_size() > 0) {
    // Encode a new message to send to the internal broadcaster.
    size_t bufferSize = response.entrant_ids_size() * sizeof(GUID) + 1;  // +1 for count byte, +1 for size byte
    auto msgData = new CHAR[bufferSize];
    memset(msgData, 0, bufferSize);
    msgData[0] = 0;  // Initialize the count of accepted players to 0
    // Iterate through the accepted entrant IDs and convert them to GUIDs
    for (const auto& entrantId : response.entrant_ids()) {
      GUID playerUuid;
      if (StringToGuid(entrantId, &playerUuid) == ERROR_SUCCESS) {
        // Add the player UUID to the array
        size_t offset = 1 + msgData[0] * sizeof(GUID);
        memcpy(msgData + offset, &playerUuid, sizeof(GUID));
        msgData[0]++;  // Increment the count of accepted players
        Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Accepted player UUID: %s", entrantId.c_str());
      } else {
        Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Failed to convert entrant ID to GUID: %s", entrantId.c_str());
      }
    }
    // Forward the received join response event to the internal broadcast.
    EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_ACCEPT_PLAYERS_SUCCESS_V2,
                                         "SNSLobbyAcceptPlayersSuccessv2", msgData, bufferSize);
  }
}

VOID lobbyEntrantsReject(GameServerLib* self, const nevr::rtapi::Envelope& envelope) {
  const auto& response = envelope.lobbyentrantsreject();
  if (response.entrant_ids_size() > 0) {  // Encode a new message to send to the internal broadcaster.
    size_t bufferSize = response.entrant_ids_size() * sizeof(GUID) + 1;  // +1 for count byte, +1 for size byte
    auto msgData = new CHAR[bufferSize];
    memset(msgData, 0, bufferSize);
    msgData[0] = 0;
    // Iterate through the rejected entrant IDs and convert them to GUIDs
    for (const auto& entrantId : response.entrant_ids()) {
      GUID playerUuid;
      if (StringToGuid(entrantId, &playerUuid) == ERROR_SUCCESS) {
        // Add the player UUID to the array
        size_t offset = 1 + msgData[0] * sizeof(GUID);
        memcpy(msgData + offset, &playerUuid, sizeof(GUID));
        msgData[0]++;  // Increment the count of accepted players
        Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Rejected player UUID: %s", entrantId.c_str());
      } else {
        Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Failed to convert entrant ID to GUID: %s", entrantId.c_str());
      }
    }
    // Forward the received event to the internal broadcast.
    EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_ACCEPT_PLAYERS_FAILURE_V2,
                                         "SNSLobbyAcceptPlayersFailurev2", msgData, bufferSize);
  }
}

VOID error(GameServerLib* self, const nevr::rtapi::Envelope& envelope) {
  // Log the error message
  const auto& error = envelope.error();
  Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Received error message: %s", error.message().c_str());

  switch (error.code()) {
    case nevr::rtapi::Error_Code_RUNTIME_EXCEPTION: {
      self->registered = FALSE;
      Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Runtime exception occurred: %s", error.message().c_str());
      // Forward the error message to the internal broadcaster
      EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_SESSION_ERROR,
                                           "SNSLobbySessionError", NULL, 0);
      break;
    }
    case nevr::rtapi::Error::Code::Error_Code_UNRECOGNIZED_PAYLOAD: {
      self->registered = FALSE;
      Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Unrecognized payload received: %s", error.message().c_str());
      // Forward the error message to the internal broadcaster
      EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_SESSION_ERROR,
                                           "SNSLobbySessionError", NULL, 0);
      break;
    }
    case nevr::rtapi::Error::Code::Error_Code_MISSING_PAYLOAD: {
      self->registered = FALSE;
      Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Missing payload in message: %s", error.message().c_str());
      // Forward the error message to the internal broadcaster
      EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_SESSION_ERROR,
                                           "SNSLobbySessionError", NULL, 0);
      break;
    }
    case nevr::rtapi::Error::Code::Error_Code_BAD_INPUT: {
      self->registered = FALSE;
      Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Bad input received: %s", error.message().c_str());
      // Forward the error message to the internal broadcaster
      EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_SESSION_ERROR,
                                           "SNSLobbySessionError", NULL, 0);
      break;
    }
    case nevr::rtapi::Error::Code::Error_Code_REGISTRATION_FAILED: {
      self->registered = FALSE;
      Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Game server registration failed: %s", error.message().c_str());
      // Forward the error message to the internal broadcaster
      CHAR* buffer = new CHAR[sizeof(BYTE)];
      memset(buffer, 0, sizeof(BYTE));
      // Set the internal code for registration failure
      BYTE internalCode = 0x0A;  // registration failure
      memcpy(buffer, &internalCode, sizeof(BYTE));
      EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_REGISTRATION_FAILURE,
                                           "SNSLobbyRegistrationFailure", buffer, sizeof(BYTE));
      break;
    }
    default:
      Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Unknown error code: %d", error.code());
      break;
  }

  // Forward the error message to the internal broadcaster
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_SESSION_ERROR,
                                       "SNSLobbySessionError", NULL, 0);
}
}  // namespace Pipeline

// Send a lobby session event to the game service.
VOID sendLobbySessionEvent(GameServerLib* self, nevr::rtapi::LobbySessionEventMessage::Code code) {
  char lobbyIdString[37];
  GuidToString(self->lobby->gameSessionId, lobbyIdString);
  nevr::rtapi::Envelope envelope;
  auto message = envelope.mutable_lobbysessionevent();
  message->set_lobby_session_id(lobbyIdString);
  message->set_code(code);
  SendProtobufMessage(self, envelope);
}

/// Deserializes a protobuf message from either binary or JSON format into an Envelope object.
/// <param name="data">Pointer to the encoded message data</param>
/// <param name="size">Size of the encoded message in bytes</param>
/// <param name="isJson">Whether the message is in JSON format</param>
/// <param name="outEnvelope">Reference to the Envelope object to be filled</param>
/// <returns>True if deserialization was successful, false otherwise</returns>
bool DecodeProtobufMessage(const void* data, UINT64 size, bool isJson, nevr::rtapi::Envelope& outEnvelope) {
  if (data == nullptr || size == 0) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Cannot decode empty protobuf message");
    return false;
  }
  bool success = false;
  if (isJson) {
    // Handle JSON format
    std::string jsonString(static_cast<const char*>(data), size);
    // Configure JSON parsing options
    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = true;
    // Parse the JSON into the protobuf message
    auto status = google::protobuf::util::JsonStringToMessage(jsonString, &outEnvelope, options);
    if (!status.ok()) {
      Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Failed to parse JSON protobuf: %s", status.ToString().c_str());
      return false;
    }
    success = true;
  } else {
    // Handle binary protobuf format
    std::string binaryData(static_cast<const char*>(data), size);
    success = outEnvelope.ParseFromString(binaryData);
    if (!success) {
      Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Failed to parse binary protobuf message");
    }
  }
  if (success) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.SERVER] Successfully decoded protobuf message");
  }
  return success;
}

VOID processProtobufMessage(GameServerLib* self, const nevr::rtapi::Envelope& envelope) {
  // Log the received protobuf message
  Log(EchoVR::LogLevel::Debug, "[NEVR.SERVER] Received protobuf message with type: %s",
      envelope.GetDescriptor()->name().c_str());
  // Process the protobuf message based on its type
  switch (envelope.message_case()) {
    case nevr::rtapi::Envelope::kError: {
      Pipeline::error(self, envelope);
      break;
    }
    case nevr::rtapi::Envelope::kLobbyEntrantsAccept: {
      Pipeline::lobbyEntrantsAccept(self, envelope);
      break;
    }
    case nevr::rtapi::Envelope::kLobbyEntrantsReject: {
      Pipeline::lobbyEntrantsReject(self, envelope);
      break;
    }
    case nevr::rtapi::Envelope::kGameServerRegistrationSuccess: {
      Pipeline::gameServerRegistrationSuccess(self, envelope);
      break;
    }
    case nevr::rtapi::Envelope::kLobbySessionCreate: {
      Pipeline::lobbySessionCreate(self, envelope);
      break;
    }
    default:
      Log(EchoVR::LogLevel::Warning, "[NEVR.SERVER] Received unhandled protobuf message type: %d",
          envelope.message_case());
      break;
  }
}

namespace pipelineWS {
// Event handler for receiving a protobuf message from the TCP (websocket)
VOID protobufMessage(GameServerLib* self, VOID* proxymthd, EchoVR::TcpPeer sender, VOID* msg, VOID* unk,
                     UINT64 msgSize) {
  nevr::rtapi::Envelope envelope;
  if (!DecodeProtobufMessage(msg, msgSize, false, envelope)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Failed to decode protobuf message");
    return;
  }
  processProtobufMessage(self, envelope);
}
// Event handler for receiving a JSON protobuf message from the TCP (websocket)
VOID protobufJsonMessage(GameServerLib* self, VOID* proxymthd, EchoVR::TcpPeer sender, VOID* msg, VOID* unk,
                         UINT64 msgSize) {
  // Decode the JSON protobuf message from the received data
  nevr::rtapi::Envelope envelope;
  if (!DecodeProtobufMessage(msg, msgSize, true, envelope)) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Failed to decode JSON protobuf message");
    return;
  }
  // Process the decoded protobuf message
  processProtobufMessage(self, envelope);
}
}  // namespace pipelineWS

// Register the game server with the game service.
VOID GameServerLib::RequestRegistration(INT64 serverId, CHAR* radId, EchoVR::SymbolId regionId,
                                        EchoVR::SymbolId versionLock, const EchoVR::Json* localConfig) {
  // Store the registration information.
  this->serverId = serverId;
  this->regionId = regionId;
  this->versionLock = versionLock;
  this->loginSessionId = GetLoginUUID();
  this->gameServerAddr = (*(sockaddr_in*)&this->broadcaster->data->addr);

  // Obtain the serverdb URI from the config (or fallback to default)
  CHAR* serverDbServiceUri = EchoVR::JsonValueAsString((EchoVR::Json*)localConfig, (CHAR*)"serverdb_host",
                                                       (CHAR*)"wss://g.echovrce.com", false);
  EchoVR::UriContainer serverDbUriContainer;
  memset(&serverDbUriContainer, 0, sizeof(serverDbUriContainer));
  if (EchoVR::UriContainerParse(&serverDbUriContainer, serverDbServiceUri) != ERROR_SUCCESS) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.SERVER] Failed to register game server: error parsing "
        "serverdb service URI");
    return;
  }

  // Connect to the serverdb websocket service
  this->tcpBroadcasterData->CreatePeer(&this->serverDbPeer, (const EchoVR::UriContainer*)&serverDbUriContainer);

  // Obtain the timestep usecs
  UINT32* timeStepUsecs = (UINT32*)(*(CHAR**)(EchoVR::g_GameBaseAddress + 0x020A00E8) + 0x90);

  // convert the GUID
  char loginSessionIdString[37];  // GUID string format: 8-4-4-4-12 + null terminator
  GuidToString(this->loginSessionId, loginSessionIdString);

  // Create the protobuf registration request
  nevr::rtapi::Envelope envelope;
  nevr::rtapi::GameServerRegistrationMessage* message = envelope.mutable_gameserverregistrationrequest();
  message->set_login_session_id(loginSessionIdString);
  message->set_server_id(this->serverId);
  message->set_port(this->broadcaster->data->broadcastSocketInfo.port);
  message->set_internal_ip_address(inet_ntoa(gameServerAddr.sin_addr));
  message->set_region(this->regionId);
  message->set_version_lock(this->versionLock);
  message->set_time_step_usecs(*timeStepUsecs);
  message->set_version(PROJECT_VERSION);

  SendProtobufMessage(this, envelope);

  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Requested game server registration");
}

/// Requests unregistration of the game server with central TCP/websocket
/// services (ServerDB). This is called by the game during game server library
/// unloading.
VOID GameServerLib::Unregister() {
  // Reset our game server library state.
  registered = FALSE;
  sessionActive = FALSE;
  serverId = -1;
  regionId = -1;
  versionLock = -1;

  // TODO: These probably aren't necessary, but it would be good to..
  // - Set lobbytype to public
  // - Clear the JSON for lobby

  // Unregister our broadcaster message listeners
  EchoVR::BroadcasterUnlisten(this->broadcaster, this->broadcastSessionStartCBHandle);
  EchoVR::BroadcasterUnlisten(this->broadcaster, this->broadcastSessionErrorCBHandle);

  EchoVR::TcpBroadcasterUnlisten(this->lobby->tcpBroadcaster, this->tcpBroadcastProtobufMessageCBHandle);
  EchoVR::TcpBroadcasterUnlisten(this->lobby->tcpBroadcaster, this->tcpBroadcastProtobufJsonMessageCBHandle);

  EchoVR::TcpBroadcasterUnlisten(this->lobby->tcpBroadcaster, this->tcpBroadcastSessionSuccessCBHandle);

  // Disconnect from server db.
  this->tcpBroadcasterData->DestroyPeer(this->serverDbPeer);

  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Unregistered game server");
}

/// Signal to the game service that the session has ended.
VOID GameServerLib::EndSession() {
  if (sessionActive) {
    sendLobbySessionEvent(this, nevr::rtapi::LobbySessionEventMessage::Code::LobbySessionEventMessage_Code_ENDED);
    // Set the timestep to a low value (6hz) while idle.
    SetTimeStepUsecs(166666);  // 6hz
    sessionActive = FALSE;
  }
}

/// Signal to the game service that player sessions should be locked. (non-authorative)
VOID GameServerLib::LockPlayerSessions() {
  if (sessionActive) {
    sendLobbySessionEvent(this, nevr::rtapi::LobbySessionEventMessage::Code::LobbySessionEventMessage_Code_LOCKED);
  }
}

/// Signal to the game service that player sessions should be unlocked. (non-authorative)
VOID GameServerLib::UnlockPlayerSessions() {
  if (sessionActive) {
    sendLobbySessionEvent(this, nevr::rtapi::LobbySessionEventMessage::Code::LobbySessionEventMessage_Code_UNLOCKED);
  }
}

/// Signal to the game service that a player has connected to the game server
VOID GameServerLib::AcceptPlayerSessions(EchoVR::Array<GUID>* playerUuids) {
  if (sessionActive) {
    nevr::rtapi::Envelope envelope;
    const auto& message = envelope.mutable_lobbyentrantconnected();
    char lobbySessionIdString[37];
    GuidToString(this->lobby->gameSessionId, lobbySessionIdString);
    message->set_lobby_session_id(lobbySessionIdString);
    // Add the player GUIDs to the message
    google::protobuf::RepeatedPtrField<std::string>* entrantids = message->mutable_entrant_ids();
    for (UINT64 i = 0; i < playerUuids->count; i++) {
      char playerUuidString[37];
      GuidToString(playerUuids->items[i], playerUuidString);
      entrantids->Add(playerUuidString);
    }
    SendProtobufMessage(this, envelope);
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SERVER] Cannot accept players, no active session");
  }
}

/// Signal to the game service that a player has been removed from the game
VOID GameServerLib::RemovePlayerSession(GUID* playerUuid) {
  if (sessionActive) {
    nevr::rtapi::Envelope envelope;
    const auto& message = envelope.mutable_lobbyentrantremoved();
    char lobbySessionIdString[37];
    GuidToString(this->lobby->gameSessionId, lobbySessionIdString);
    message->set_lobby_session_id(lobbySessionIdString);
    char playerUuidString[37];
    GuidToString(*playerUuid, playerUuidString);
    message->set_entrant_id(playerUuidString);
    SendProtobufMessage(this, envelope);
    Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Removed player session: %s", playerUuidString);
  }
}

/// Initializes the game server library. This is called by the game after the
/// library has been loaded.
/// <param name="lobby">The current game lobby structure to reference/leverage
/// when operating the game server.</param> <param name="broadcaster">The
/// internal game server broadcast to use to communicate with clients.</param>
/// <param name="unk2">TODO: Unknown.</param>
/// <param name="logPath">The file path where the current log file
/// resides.</param> <returns>None</returns>
VOID* GameServerLib::Initialize(EchoVR::Lobby* lobby, EchoVR::Broadcaster* broadcaster, VOID* unk2,
                                const CHAR* logPath) {
  // Verify the protobuf version.
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Log the server version
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] version %s (%s) initializing...", PROJECT_VERSION, GIT_COMMIT_HASH);

  // Set up our game server state.
  this->lobby = lobby;
  this->broadcaster = broadcaster;
  this->tcpBroadcasterData = lobby->tcpBroadcaster->data;
  this->defaultTimeStepUsecs = GetTimeStepUsecs();  // Get the timestep set during game initialization.

  // Subscribe to broadcaster events
  this->broadcastSessionStartCBHandle = ListenForBroadcasterMessage(this, SYMBOL_BROADCASTER_LOBBY_SESSION_STARTING,
                                                                    TRUE, (VOID*)internalPipeline::sessionStarting);
  this->broadcastSessionErrorCBHandle = ListenForBroadcasterMessage(this, SYMBOL_BROADCASTER_LOBBY_SESSION_ERROR, TRUE,
                                                                    (VOID*)internalPipeline::sessionError);

  // Subscribe to websocket events.
  this->tcpBroadcastSessionSuccessCBHandle = ListenForTcpBroadcasterMessage(
      this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_SUCCESS_V5, (VOID*)OnTcpMsgPlayerConnectDetails);

  this->tcpBroadcastProtobufMessageCBHandle = ListenForTcpBroadcasterMessage(
      this, SYMBOL_TCPBROADCASTER_NEVRPROTOBUF_MESSAGE_V1, (VOID*)pipelineWS::protobufMessage);

  this->tcpBroadcastProtobufJsonMessageCBHandle = ListenForTcpBroadcasterMessage(
      this, SYMBOL_TCPBROADCASTER_NEVRPROTOBUF_JSON_MESSAGE_V1, (VOID*)pipelineWS::protobufJsonMessage);

  // Log the interaction.
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Initialized game server");
  // lobby->hosting |= 0x1;

#if _DEBUG
  Log(EchoVR::LogLevel::Debug, "[NEVR.SERVER] EchoVR base address = 0x%p", (VOID*)EchoVR::g_GameBaseAddress);
#endif

  // This should return a valid pointer to simply dereference.
  return this;
}

/// Terminates the game server library. This is called by the game prior to
/// unloading the library.
VOID GameServerLib::Terminate() { Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Terminated game server"); }

/// Updates the game server library. This is called by the game at a frequent
/// interval.
VOID GameServerLib::Update() {
  // TODO: This is temporary code to test if the profile JSON is updated (but
  // not sent to server). If it is not updated in this structure, one of the
  // "apply loadout" or "save loadout" operations may trigger the update?
  for (int i = 0; i < this->lobby->entrantData.count; i++) {
    // Obtain the entrant at the given index.
    EchoVR::Lobby::EntrantData* entrantData = (this->lobby->entrantData.items + i);

    // TODO: If the entrant is marked dirty...
    if (entrantData->userId.accountId != 0 && entrantData->dirty) {
    }
  }
}

/// TODO: Unknown. This is called during initialization with a value of 6. Maybe
/// it is platform/game server privilege/role related?
/// <param name="unk">TODO: Unknown.</param>
VOID GameServerLib::UnkFunc1(UINT64 unk) {
  // Note: This function is called prior to Initialize.
}
