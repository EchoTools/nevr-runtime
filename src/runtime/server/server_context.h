#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include "abi/echovr.h"
#include "core/pch.h"

namespace GameServer {

// Server lifecycle states
enum class ServerState : int32_t {
  Uninitialized = 0,
  Initialized = 1,
  Registered = 2,
  InSession = 3,
  Terminated = 4,
};

// Callback registration handles for broadcaster events
struct CallbackRegistry {
  // Broadcaster that owns the UDP callback handles below. This remains stable
  // until UnregisterAllCallbacks has removed those handles.
  EchoVR::Broadcaster* broadcasterOwner = nullptr;

  // Internal broadcaster (UDP) callbacks
  uint16_t sessionStart = 0;
  uint16_t sessionError = 0;
  uint16_t saveLoadout = 0;
  uint16_t saveLoadoutSuccess = 0;
  uint16_t saveLoadoutPartial = 0;
  uint16_t currentLoadoutRequest = 0;
  uint16_t currentLoadoutResponse = 0;
  uint16_t refreshProfileForUser = 0;
  uint16_t refreshProfileFromServer = 0;
  uint16_t lobbySendClientSettings = 0;
  uint16_t tierReward = 0;
  uint16_t topAwards = 0;
  uint16_t newUnlocks = 0;
  uint16_t reliableStatUpdate = 0;
  uint16_t reliableTeamStatUpdate = 0;

  // TCP broadcaster (websocket) callbacks
  uint16_t tcpRegSuccess = 0;
  uint16_t tcpRegFailure = 0;
  uint16_t tcpSessionSuccess = 0;
  uint16_t tcpProtobuf = 0;

  void Clear();
};

// Session-related state (changes during gameplay)
struct SessionState {
  bool active = false;
  GUID loginSessionId = {};
  std::string lobbySessionId;  // UUID string from LobbySessionSuccessV5
  uint64_t serverId = 0;
  EchoVR::SymbolId regionId = 0;
  EchoVR::SymbolId versionLock = 0;
  sockaddr_in gameServerAddr = {};
  uint32_t defaultTimeStepUsecs = 0;

  void Reset();
};

// Thread-safe server context managing lobby access and state
class ServerContext {
 public:
  ServerContext() = default;
  ~ServerContext() = default;

  // Non-copyable, non-movable (owns mutex)
  ServerContext(const ServerContext&) = delete;
  ServerContext& operator=(const ServerContext&) = delete;
  ServerContext(ServerContext&&) = delete;
  ServerContext& operator=(ServerContext&&) = delete;

  // Initialization (exclusive lock)
  void Initialize(EchoVR::Lobby* lobby, EchoVR::Broadcaster* broadcaster);
  void FinalizeInitialization();
  void Terminate();

  // State transitions (exclusive lock)
  bool SetRegistered(bool registered);
  bool StartSession();
  bool EndSession();

  // State queries (shared lock for reads)
  ServerState GetState() const;
  bool IsInitialized() const;
  bool IsRegistered() const;
  bool IsSessionActive() const;
  bool IsValidForOperations() const;  // registered && sessionActive

  // Lobby access (shared lock for reads)
  // Returns nullptr if not initialized
  EchoVR::Lobby* GetLobby() const;
  EchoVR::Broadcaster* GetBroadcaster() const;
  EchoVR::TcpBroadcasterData* GetTcpBroadcaster() const;

  // Entrant access, read live from the lobby's entrant array on every call
  // (issue #38). Returns nullptr if index is out of bounds or not initialized.
  // The pointer is into game memory: use it within the current game-thread
  // callback and never store it — the game may free or rewrite the array on
  // its next tick.
  EchoVR::Lobby::EntrantData* GetEntrant(uint32_t index) const;
  uint64_t GetEntrantCount() const;

  // Resolves an entrant's session GUID to its entrant slot the way the game does for accepts
  // (echovr.exe 0x140603e20): the index of the lobby's player-session slot (lobby+0xC8, stride 0x28)
  // whose GUID matches and whose join state is 4 (accepted), within the entrant array's length. Returns
  // false when not initialized, the array is absent, or nothing matches.
  bool FindEntrantSlotBySession(const GUID& session, uint64_t& slot) const;

  // Number of player sessions whose join state is 4 (accepted): the players actually in the session.
  // GetEntrantCount() is the array capacity, not this. 0 when not initialized.
  uint64_t CountAcceptedEntrants() const;

  // ServerDB peer (exclusive lock for write)
  void SetServerDbPeer(const EchoVR::TcpPeer& peer);
  EchoVR::TcpPeer GetServerDbPeer() const;

  // Session state access (uses separate mutex for read-heavy patterns)
  SessionState GetSessionState() const;
  void UpdateSessionState(const SessionState& state);

  // Callback registry — NOT internally synchronized.
  // Safe to call without locking when all access is from the game's main thread
  // (RegisterBroadcasterCallbacks, UnregisterAllCallbacks, Initialize, Terminate).
  // Must not be called from ixwebsocket or other background threads. A background
  // thread that needs registry work hands it to the game thread instead
  // (GameServerLib::m_gameThreadHandoff, serviced in Update(); GH #44).
  CallbackRegistry& GetCallbackRegistry();
  const CallbackRegistry& GetCallbackRegistry() const;

 private:
  mutable std::shared_mutex m_stateMutex;  // Protects m_state and pointers
  mutable std::mutex m_sessionMutex;       // Protects m_sessionState

  ServerState m_state = ServerState::Uninitialized;

  // Game object pointers (not owned, provided by game engine)
  EchoVR::Lobby* m_lobby = nullptr;
  EchoVR::Broadcaster* m_broadcaster = nullptr;

  // ServerDB connection
  EchoVR::TcpPeer m_serverDbPeer = EchoVR::TcpPeer_InvalidPeer;

  // Session state
  SessionState m_sessionState;

  // Callback handles
  CallbackRegistry m_callbacks;
};

}  // namespace GameServer
