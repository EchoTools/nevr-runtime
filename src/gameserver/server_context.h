#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "echovr.h"
#include "pch.h"

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

  // Safe entrant access with bounds checking (shared lock)
  // Returns nullptr if index out of bounds or not initialized
  EchoVR::Lobby::EntrantData* GetEntrant(uint32_t index) const;
  uint64_t GetEntrantCount() const;

  // ServerDB peer (exclusive lock for write)
  void SetServerDbPeer(const EchoVR::TcpPeer& peer);
  EchoVR::TcpPeer GetServerDbPeer() const;

  // Session state access (uses separate mutex for read-heavy patterns)
  SessionState GetSessionState() const;
  void UpdateSessionState(const SessionState& state);

  // Callback registry (exclusive lock)
  CallbackRegistry& GetCallbackRegistry();
  const CallbackRegistry& GetCallbackRegistry() const;

 private:
  mutable std::shared_mutex m_stateMutex;  // Protects m_state and pointers
  mutable std::mutex m_sessionMutex;       // Protects m_sessionState

  ServerState m_state = ServerState::Uninitialized;

  // Game object pointers (not owned, provided by game engine)
  EchoVR::Lobby* m_lobby = nullptr;
  EchoVR::Broadcaster* m_broadcaster = nullptr;

  // Cached entrant data (owns the data, not pointers from game)
  std::vector<EchoVR::Lobby::EntrantData> m_cachedEntrants;

  // ServerDB connection
  EchoVR::TcpPeer m_serverDbPeer = EchoVR::TcpPeer_InvalidPeer;

  // Session state
  SessionState m_sessionState;

  // Callback handles
  CallbackRegistry m_callbacks;
};

}  // namespace GameServer
