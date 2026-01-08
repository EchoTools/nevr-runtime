#include "gameserver.h"

#include <cstdio>
#include <cstring>

#include "constants.h"
#include "echovr.h"
#include "echovrunexported.h"
#include "messages.h"
#include "pch.h"

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

// Send message to ServerDB via TCP broadcaster
void SendServerdbTcpMessage(GameServerLib* self, EchoVR::SymbolId msgId, VOID* msg, uint64_t msgSize) {
  auto* tcp = self->GetContext().GetTcpBroadcaster();
  if (!tcp) return;

  auto peer = self->GetContext().GetServerDbPeer();
  tcp->SendToPeer(peer, msgId, nullptr, 0, msg, msgSize);
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
  auto* broadcaster = self->GetContext().GetBroadcaster();
  if (broadcaster) {
    EchoVR::BroadcasterReceiveLocalEvent(broadcaster, Sym::LobbySessionSuccessV5, "SNSLobbySessionSuccessv5",
                                         static_cast<CHAR*>(msg), msgSize);
  }
}

// --- Internal Broadcaster Callbacks ---

void OnMsgSessionStarting(GameServerLib* self, VOID*, VOID*, UINT64, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Session starting");
}

void OnMsgSessionError(GameServerLib* self, VOID*, VOID*, UINT64, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] Session error encountered");
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

  // Log player info if available
  auto* entrant = self->GetContext().GetEntrant(slot.slot);
  if (entrant) {
    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Player: %s (%s)", entrant->displayName,
        entrant->uniqueName);
  }

  // Forward to game service if session is active
  if (self->GetContext().IsValidForOperations()) {
    SendServerdbTcpMessage(self, TcpSym::SaveLoadoutRequest, msg, msgSize);
    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Forwarded %llu bytes to game service", msgSize);
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] [SAVE_LOADOUT] Not in active session");
  }
}

void OnMsgSaveLoadoutSuccess(GameServerLib*, VOID*, VOID*, UINT64 msgSize, EchoVR::Peer, EchoVR::Peer) {
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Save loadout success (size: %llu)", msgSize);
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

  cb.tcpRegSuccess = ListenForTcpBroadcasterMessage(this, TcpSym::LobbyRegistrationSuccess,
                                                    reinterpret_cast<VOID*>(OnTcpMsgRegistrationSuccess));
  cb.tcpRegFailure = ListenForTcpBroadcasterMessage(this, TcpSym::LobbyRegistrationFailure,
                                                    reinterpret_cast<VOID*>(OnTcpMsgRegistrationFailure));
  cb.tcpStartSession = ListenForTcpBroadcasterMessage(this, TcpSym::LobbyStartSession,
                                                      reinterpret_cast<VOID*>(OnTcpMessageStartSession));
  cb.tcpPlayersAccepted = ListenForTcpBroadcasterMessage(this, TcpSym::LobbyPlayersAccepted,
                                                         reinterpret_cast<VOID*>(OnTcpMsgPlayersAccepted));
  cb.tcpPlayersRejected = ListenForTcpBroadcasterMessage(this, TcpSym::LobbyPlayersRejected,
                                                         reinterpret_cast<VOID*>(OnTcpMsgPlayersRejected));
  cb.tcpSessionSuccess = ListenForTcpBroadcasterMessage(this, TcpSym::LobbySessionSuccessV5,
                                                        reinterpret_cast<VOID*>(OnTcpMsgSessionSuccessv5));
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
    EchoVR::TcpBroadcasterUnlisten(lobby->tcpBroadcaster, cb.tcpRegSuccess);
    EchoVR::TcpBroadcasterUnlisten(lobby->tcpBroadcaster, cb.tcpRegFailure);
    EchoVR::TcpBroadcasterUnlisten(lobby->tcpBroadcaster, cb.tcpStartSession);
    EchoVR::TcpBroadcasterUnlisten(lobby->tcpBroadcaster, cb.tcpPlayersAccepted);
    EchoVR::TcpBroadcasterUnlisten(lobby->tcpBroadcaster, cb.tcpPlayersRejected);
    EchoVR::TcpBroadcasterUnlisten(lobby->tcpBroadcaster, cb.tcpSessionSuccess);
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
                                const_cast<CHAR*>("ws://localhost:777/serverdb"), false);

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

  ERLobbyRegistrationRequest request = {};
  request.serverId = serverId;
  request.port = static_cast<uint16_t>(broadcaster->data->broadcastSocketInfo.port);
  request.internalIp = gameServerAddr.sin_addr.S_un.S_addr;
  request.regionId = regionId;
  request.versionLock = versionLock;

  SendServerdbTcpMessage(this, TcpSym::LobbyRegistrationRequest, &request, sizeof(request));

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Requested game server registration");
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
    ERLobbyEndSession message = {};
    SendServerdbTcpMessage(this, TcpSym::LobbyEndSession, &message, sizeof(message));
  }

  context_->EndSession();
  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Signaling end of session");
}

VOID GameServerLib::LockPlayerSessions() {
  if (context_->IsSessionActive()) {
    ERLobbyPlayerSessionsLocked message = {};
    SendServerdbTcpMessage(this, TcpSym::LobbyPlayerSessionsLocked, &message, sizeof(message));
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Signaling game server locked");
}

VOID GameServerLib::UnlockPlayerSessions() {
  if (context_->IsSessionActive()) {
    ERLobbyPlayerSessionsUnlocked message = {};
    SendServerdbTcpMessage(this, TcpSym::LobbyPlayerSessionsUnlocked, &message, sizeof(message));
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Signaling game server unlocked");
}

VOID GameServerLib::AcceptPlayerSessions(EchoVR::Array<GUID>* playerUuids) {
  if (context_->IsSessionActive()) {
    SendServerdbTcpMessage(this, TcpSym::LobbyAcceptPlayers, playerUuids->items, playerUuids->count * sizeof(GUID));
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Accepted %d players", playerUuids->count);
}

VOID GameServerLib::RemovePlayerSession(GUID* playerUuid) {
  if (context_->IsSessionActive()) {
    SendServerdbTcpMessage(this, TcpSym::LobbyRemovePlayer, playerUuid, sizeof(GUID));
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Removed player from game server");
}
