#include "gameserver.h"

#include <cstdio>
#include <vector>

#include "echovr.h"
#include "echovrunexported.h"
#include "messages.h"
#include "pch.h"

// Define the version number
// Get the version number from CMake-generated definition
#ifndef VERSION
#define VERSION "1.0.0"  // Fallback default version
#endif

#ifndef BUILD_ID
#define BUILD_ID "unknown"  // Fallback default build ID
#endif

/// <summary>
/// A wrapper for WriteLog, simplifying logging operations.
/// </summary>
/// <returns>None</returns>
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

/// <summary>
/// Subscribes to internal local events for a given message type. These are
/// typically sent internally by the game to its self, or derived from connected
/// peer's messages (UDP broadcast port forwards events). The provided function
/// is used as a callback when a message of the type is received from any peer
/// or onesself.
/// </summary>
/// <param name="self">The game server library which is listening for the
/// message.</param> <param name="msgId">The 64-bit symbol used to describe a
/// message type/identifier to listen for.</param> <param
/// name="isMsgReliable">Indicates whether we are listening for events for
/// messages sent over the reliable or mailbox game server message inbox
/// types.</param> <param name="func">The function to use as callback when a
/// broadcaster message of the given type is received.</param> <returns>An
/// identifier/handle for the callback registration, to be later used in
/// unregistering.</returns>
UINT16 ListenForBroadcasterMessage(GameServerLib* self, EchoVR::SymbolId msgId, BOOL isMsgReliable, VOID* func) {
  // Subscribe to the provided message id.
  EchoVR::DelegateProxy listenerProxy;
  memset(&listenerProxy, 0, sizeof(listenerProxy));
  listenerProxy.method[0] = 0xFFFFFFFFFFFFFFFF;
  listenerProxy.instance = (VOID*)self;
  listenerProxy.proxyFunc = func;

  return EchoVR::BroadcasterListen(self->lobby->broadcaster, msgId, isMsgReliable, (VOID*)&listenerProxy, true);
}

/// <summary>
/// Subscribes the TCP broadcasters (websocket) to a given message type. The
/// provided function is used as a callback when a message of the type is
/// received from any service.
/// </summary>
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

/// <summary>
/// Sends a message using the TCP broadcaster to the ServerDB websocket service.
/// </summary>
/// <param name="self">The game server library which is sending the message to
/// the service.</param> <param name="msgId">The 64-bit symbol used to describe
/// the message type/identifier being sent.</param> <param name="msg">A pointer
/// to the message data to be sent.</param> <param name="msgSize">The size of
/// the msg to be sent, in bytes.</param> <returns>None</returns>
VOID SendServerdbTcpMessage(GameServerLib* self, EchoVR::SymbolId msgId, VOID* msg, UINT64 msgSize) {
  // Wrap the send call provided by the TCP broadcaster.
  self->tcpBroadcasterData->SendToPeer(self->serverDbPeer, msgId, NULL, 0, msg, msgSize);
}

/// <summary>
/// Event handler for receiving a game server registration success message from
/// the TCP (websocket) ServerDB service. This message indicates the game server
/// registration with ServerDB was accepted.
/// </summary>
/// <returns>None</returns>
VOID OnTcpMsgRegistrationSuccess(GameServerLib* self, VOID* proxymthd, EchoVR::TcpPeer sender, VOID* msg, VOID* unk,
                                 UINT64 msgSize) {
  // Set the registration status
  self->registered = TRUE;

  // Forward the received registration success event to the internal broadcast.
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_REGISTRATION_SUCCESS,
                                       "SNSLobbyRegistrationSuccess", msg, msgSize);
}

/// <summary>
/// Event handler for receiving a game server registration failure message from
/// the TCP (websocket) ServerDB service. This message indicates the game server
/// registration with ServerDB was rejected.
/// </summary>
/// <returns>None</returns>
VOID OnTcpMsgRegistrationFailure(GameServerLib* self, VOID* proxymthd, EchoVR::TcpPeer sender, VOID* msg, VOID* unk,
                                 UINT64 msgSize) {
  // Set the registration status
  self->registered = FALSE;

  // Forward the received registration failure event to the internal broadcast.
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_REGISTRATION_FAILURE,
                                       "SNSLobbyRegistrationFailure", msg, msgSize);
}

/// <summary>
/// Event handler for receiving a start session request from the TCP (websocket)
/// ServerDB service. This message directs the game to start loading a new game
/// session with the provided request arguments.
/// </summary>
/// <returns>None</returns>
VOID OnTcpMsgStartSession(GameServerLib* self, VOID* proxymthd, EchoVR::TcpPeer sender, VOID* msg, VOID* unk,
                          UINT64 msgSize) {
  // Set our session to active.
  self->sessionActive = TRUE;

  // Forward the received start session event to the internal broadcast.
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Starting new session");
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_START_SESSION_V4,
                                       "SNSLobbyStartSessionv4", msg, msgSize);
}

// Message from game service that the player has been accepted into the
// session, and the game server should accept them.
VOID OnTcpMsgAcceptPlayerSession(GameServerLib* self, VOID* proxymthd, EchoVR::TcpPeer sender, VOID* msg, VOID* unk,
                                 UINT64 msgSize) {
  // Forward the received player acceptance success event to the internal
  // broadcast.
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_ACCEPT_PLAYERS_SUCCESS_V2,
                                       "SNSLobbyAcceptPlayersSuccessv2", msg, msgSize);
}

// Message from game service that the player has been rejected from the
// session, and the game server should reject them.
VOID OnTcpMsgRemovePlayerSession(GameServerLib* self, VOID* proxymthd, EchoVR::TcpPeer sender, VOID* msg, VOID* unk,
                                 UINT64 msgSize) {
  // Forward the received player acceptance failure event to the internal
  // broadcast.
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_ACCEPT_PLAYERS_FAILURE_V2,
                                       "SNSLobbyAcceptPlayersFailurev2", msg, msgSize);
}

// Message from game service with connection parameters for player-server communication
VOID OnTcpMsgPlayerConnectDetails(GameServerLib* self, VOID* proxymthd, EchoVR::TcpPeer sender, VOID* msg, VOID* unk,
                                  UINT64 msgSize) {
  // Forward the received join session success event to the internal broadcast.
  EchoVR::BroadcasterReceiveLocalEvent(self->broadcaster, SYMBOL_BROADCASTER_LOBBY_SESSION_SUCCESS_V5,
                                       "SNSLobbySessionSuccessv5", (CHAR*)msg, msgSize);
}

/// <summary>
/// Event handler for receiving a session start success message from events
/// (internal game server broadcast). This message indicates that a new session
/// is starting.This is triggered after a SNSLobbyStartSessionv4 message is
/// processed.
/// </summary>
/// <returns>None</returns>
VOID OnMsgSessionStarting(GameServerLib* self, VOID* proxymthd, VOID* msg, UINT64 msgSize, EchoVR::Peer destination,
                          EchoVR::Peer sender) {
  // NOTE: `msg` here has no substance (one uninitialized byte).
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Session starting");
}

/// <summary>
/// Event handler for receiving a session error message from events (internal
/// game server broadcast). This message indicates that the game session
/// encountered an error either when starting or running.
/// </summary>
/// <returns>None</returns>
VOID OnMsgSessionError(GameServerLib* self, VOID* proxymthd, VOID* msg, UINT64 msgSize, EchoVR::Peer destination,
                       EchoVR::Peer sender) {
  // NOTE: `msg` here has no substance (one uninitialized byte).
  Log(EchoVR::LogLevel::Error, "[NEVR.SERVER] Session error encountered");
}

/// <summary>
/// TODO: This vtable slot is not verified to be for this purpose.
/// In any case, it seems not to be called or problematic, so we'll leave this
/// definition as a placeholder.
/// </summary>
/// <param name="unk1">TODO: Unknown</param>
/// <param name="a2">TODO: Unknown</param>
/// <param name="a3">TODO: Unknown</param>
/// <returns>TODO: Unknown</returns>
INT64 GameServerLib::UnkFunc0(VOID* unk1, INT64 a2, INT64 a3) { return 1; }

/// <summary>
/// Initializes the game server library. This is called by the game after the
/// library has been loaded.
/// </summary>
/// <param name="lobby">The current game lobby structure to reference/leverage
/// when operating the game server.</param> <param name="broadcaster">The
/// internal game server broadcast to use to communicate with clients.</param>
/// <param name="unk2">TODO: Unknown.</param>
/// <param name="logPath">The file path where the current log file
/// resides.</param> <returns>None</returns>
VOID* GameServerLib::Initialize(EchoVR::Lobby* lobby, EchoVR::Broadcaster* broadcaster, VOID* unk2,
                                const CHAR* logPath) {
  // Log the server version
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] version %s (%s) initializing...", VERSION, BUILD_ID);

  // Set up our game server state.
  this->lobby = lobby;
  this->broadcaster = broadcaster;
  this->tcpBroadcasterData = lobby->tcpBroadcaster->data;

  // Subscribe to broadcaster events
  this->broadcastSessionStartCBHandle =
      ListenForBroadcasterMessage(this, SYMBOL_BROADCASTER_LOBBY_SESSION_STARTING, TRUE, (VOID*)OnMsgSessionStarting);
  this->broadcastSessionErrorCBHandle =
      ListenForBroadcasterMessage(this, SYMBOL_BROADCASTER_LOBBY_SESSION_ERROR, TRUE, (VOID*)OnMsgSessionError);

  // Subscribe to websocket events.
  this->tcpBroadcastRegSuccessCBHandle = ListenForTcpBroadcasterMessage(
      this, SYMBOL_TCPBROADCASTER_LOBBY_REGISTRATION_SUCCESS, (VOID*)OnTcpMsgRegistrationSuccess);
  this->tcpBroadcastRegFailureCBHandle = ListenForTcpBroadcasterMessage(
      this, SYMBOL_TCPBROADCASTER_LOBBY_REGISTRATION_FAILURE, (VOID*)OnTcpMsgRegistrationFailure);
  this->tcpBroadcastStartSessionCBHandle =
      ListenForTcpBroadcasterMessage(this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_START_V1, (VOID*)OnTcpMsgStartSession);
  this->tcpBroadcastPlayersAcceptedCBHandle = ListenForTcpBroadcasterMessage(
      this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_ENTRANT_ACCEPT_V1, (VOID*)OnTcpMsgAcceptPlayerSession);
  this->tcpBroadcastPlayersRejectedCBHandle = ListenForTcpBroadcasterMessage(
      this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_ENTRANT_REJECT_V1, (VOID*)OnTcpMsgRemovePlayerSession);
  this->tcpBroadcastSessionSuccessCBHandle = ListenForTcpBroadcasterMessage(
      this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_SUCCESS_V5, (VOID*)OnTcpMsgPlayerConnectDetails);

  // Log the interaction.
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Initialized game server");
// lobby->hosting |= 0x1;

// If we built the module in debug mode, print the base address into logs for
// debugging purposes.
#if _DEBUG
  Log(EchoVR::LogLevel::Debug, "[NEVR.SERVER] EchoVR base address = 0x%p", (VOID*)EchoVR::g_GameBaseAddress);
#endif

  // This should return a valid pointer to simply dereference.
  return this;
}

/// <summary>
/// Terminates the game server library. This is called by the game prior to
/// unloading the library.
/// </summary>
/// <returns>None</returns>
VOID GameServerLib::Terminate() { Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Terminated game server"); }

/// <summary>
/// Updates the game server library. This is called by the game at a frequent
/// interval.
/// </summary>
/// <returns>None</returns>
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

/// <summary>
/// TODO: Unknown. This is called during initialization with a value of 6. Maybe
/// it is platform/game server privilege/role related?
/// </summary>
/// <param name="unk">TODO: Unknown.</param>
/// <returns>None</returns>
VOID GameServerLib::UnkFunc1(UINT64 unk) {
  // Note: This function is called prior to Initialize.
}

// Register the game server with the game service.
VOID GameServerLib::RequestRegistration(INT64 serverId, CHAR* radId, EchoVR::SymbolId regionId,
                                        EchoVR::SymbolId versionLock, const EchoVR::Json* localConfig) {
  // Store the registration information.
  this->serverId = serverId;
  this->regionId = regionId;
  this->versionLock = versionLock;
  this->loginSessionId = GetLoginUUID();
  this->gameServerAddr = (*(sockaddr_in*)&this->broadcaster->data->addr);

  // Obtain the serverdb URI from our config (or fallback to default)
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

  // TODO: Default port

  // Connect to the serverdb websocket service
  this->tcpBroadcasterData->CreatePeer(&this->serverDbPeer, (const EchoVR::UriContainer*)&serverDbUriContainer);

  // Obtain the timestep usecs
  UINT32* timeStepUsecs = (UINT32*)(*(CHAR**)(EchoVR::g_GameBaseAddress + 0x020A00E8) + 0x90);

  // Construct the registration request message.
  NEVRLobbyRegistrationRequestV1 regRequest;
  regRequest.loginUUID = this->loginSessionId;
  regRequest.serverId = this->serverId;
  regRequest.port = (UINT16)this->broadcaster->data->broadcastSocketInfo.port;
  regRequest.internalIp = gameServerAddr.sin_addr.S_un.S_addr;
  regRequest.regionId = this->regionId;
  regRequest.versionLock = this->versionLock;
  regRequest.timestepUSecs = *timeStepUsecs;

  // Send the registration request.
  SendServerdbTcpMessage(this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_REGISTRATION_REQUEST_V1, &regRequest,
                         sizeof(regRequest));

  // Log the interaction.
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Requested game server registration");
}

/// <summary>
/// Requests unregistration of the game server with central TCP/websocket
/// services (ServerDB). This is called by the game during game server library
/// unloading.
/// </summary>
/// <returns>None</returns>
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

  EchoVR::TcpBroadcasterUnlisten(this->lobby->tcpBroadcaster, this->tcpBroadcastRegSuccessCBHandle);
  EchoVR::TcpBroadcasterUnlisten(this->lobby->tcpBroadcaster, this->tcpBroadcastRegFailureCBHandle);
  EchoVR::TcpBroadcasterUnlisten(this->lobby->tcpBroadcaster, this->tcpBroadcastStartSessionCBHandle);
  EchoVR::TcpBroadcasterUnlisten(this->lobby->tcpBroadcaster, this->tcpBroadcastPlayersAcceptedCBHandle);
  EchoVR::TcpBroadcasterUnlisten(this->lobby->tcpBroadcaster, this->tcpBroadcastPlayersRejectedCBHandle);
  EchoVR::TcpBroadcasterUnlisten(this->lobby->tcpBroadcaster, this->tcpBroadcastSessionSuccessCBHandle);

  // Disconnect from server db.
  this->tcpBroadcasterData->DestroyPeer(this->serverDbPeer);

  // Log the interaction.
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Unregistered game server");
}

/// <summary>
/// Signals the ending of the current game server session with central
/// TCP/websocket services (ServerDB). This is called by the game when a game
/// has ended, or a session was started but no players joined for some time,
/// causing the game server to load back to the mainmenu, awaiting further
/// orders from ServerDB.
/// </summary>
/// <returns>None</returns>
VOID GameServerLib::EndSession() {
  // If there is a running session, inform the websocket so it can track the
  // state change.
  if (sessionActive) {
    NEVRLobbySessionEndedV1 message;
    message.lobbySessionId = this->lobbySessionId;
    SendServerdbTcpMessage(this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_ENDED_V1, &message, sizeof(message));
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Signaling end of session");
}

/// <summary>
/// Signals the locking of player sessions in the current game server session
/// with central TCP/websocket services (ServerDB), indicating players should no
/// longer be able to join the current game session. This is called by the game
/// after a game has been started by some time, to avoid players from joining
/// during the later halves of game sessions.
/// </summary>
/// <returns>None</returns>
VOID GameServerLib::LockPlayerSessions() {
  // If there is a running session, inform the websocket so it can track the
  // state change.
  if (sessionActive) {
    NEVRLobbySessionLockV1 message;
    message.lobbySessionId = this->lobbySessionId;
    SendServerdbTcpMessage(this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_LOCK_V1, &message, sizeof(message));
  }

  // Log the interaction.
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Signaling session lock event");
}

/// <summary>
/// Signals the unlocking of player sessions in the current game server session
/// with central TCP/websocket services (ServerDB), indicating players should be
/// able to join the current game session.
/// </summary>
/// <returns>None</returns>
VOID GameServerLib::UnlockPlayerSessions() {
  // If there is a running session, inform the websocket so it can track the
  // state change.
  if (sessionActive) {
    NEVRLobbySessionUnlockV1 message;
    message.lobbySessionId = this->lobbySessionId;
    SendServerdbTcpMessage(this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_UNLOCK_V1, &message, sizeof(message));
  }

  // Log the interaction.
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Signaling session unlock event");
}

// Signal to the game service that a player has connected to the game server
VOID GameServerLib::AcceptPlayerSessions(EchoVR::Array<GUID>* playerUuids) {
  // If we have an active session, signal to serverdb that we are accepting the
  // provided player UUIDs.
  if (sessionActive) {
    NEVRLobbyEntrantJoinAttemptV1 message;
    message.lobbySessionId = this->lobbySessionId;
    message.playerCount = playerUuids->count;
    message.playerUuids = playerUuids;

    SendServerdbTcpMessage(this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_ENTRANT_JOIN_ATTEMPT_V1, &message,
                           sizeof(message) - sizeof(EchoVR::Array<GUID>*) + playerUuids->count * sizeof(GUID));
  } else {
    // TODO: Receive local event "SNSLobbyAcceptPlayersFailurev2"
  }

  // Log the interaction.
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Accepted %d players into game server", playerUuids->count);
}

/// <summary>
/// Signals rejection of player sessions by the game server, with central
/// TCP/websocket services (ServerDB). This is called by the game after a peer
/// connects to the game server, but the game server did not accept them,
/// possibly due to inability to communicate(e.g.invalid packet encoder settings
/// for one party), or due to general communication / peer state errors.
/// </summary>
/// <param name="playerUuid">A single player session UUID which have been
/// removed by the game server.</param> <returns>None</returns>
VOID GameServerLib::RemovePlayerSession(GUID* playerUuid) {
  // If we have an active session, signal to serverdb that we are removing the
  // provided player UUID.
  if (sessionActive) {
    NEVRLobbyEntrantRemovedV1 message;
    message.lobbySessionId = this->lobbySessionId;
    message.playerUuid = *playerUuid;
    SendServerdbTcpMessage(this, SYMBOL_TCPBROADCASTER_LOBBY_SESSION_ENTRANT_REMOVED_V1, &message, sizeof(message));
  }

  // Log the interaction.
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVER] Removed a player from game server");
}
