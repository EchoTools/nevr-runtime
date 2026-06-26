#include "server_context.h"

namespace GameServer {

// CallbackRegistry implementation
void CallbackRegistry::Clear() {
  sessionStart = 0;
  sessionError = 0;
  saveLoadout = 0;
  saveLoadoutSuccess = 0;
  saveLoadoutPartial = 0;
  currentLoadoutRequest = 0;
  currentLoadoutResponse = 0;
  refreshProfileForUser = 0;
  refreshProfileFromServer = 0;
  lobbySendClientSettings = 0;
  tierReward = 0;
  topAwards = 0;
  newUnlocks = 0;
  reliableStatUpdate = 0;
  reliableTeamStatUpdate = 0;

  tcpRegSuccess = 0;
  tcpRegFailure = 0;
  tcpSessionSuccess = 0;
  tcpProtobuf = 0;
}

// SessionState implementation
void SessionState::Reset() {
  active = false;
  loginSessionId = {};
  lobbySessionId.clear();
  serverId = 0;
  regionId = 0;
  versionLock = 0;
  gameServerAddr = {};
  defaultTimeStepUsecs = 0;
}

// ServerContext implementation
void ServerContext::Initialize(EchoVR::Lobby* lobby, EchoVR::Broadcaster* broadcaster) {
  std::unique_lock lock(m_stateMutex);

  if (lobby && lobby->entrantData.items) {
    m_cachedEntrants.clear();
    m_cachedEntrants.reserve(lobby->entrantData.count);
    for (uint64_t i = 0; i < lobby->entrantData.count; ++i) {
      m_cachedEntrants.push_back(lobby->entrantData.items[i]);
    }
  }

  m_lobby = lobby;
  m_broadcaster = broadcaster;

  m_serverDbPeer = EchoVR::TcpPeer_InvalidPeer;

  {
    std::lock_guard sessionLock(m_sessionMutex);
    m_sessionState.Reset();
  }

  m_callbacks.Clear();
}

void ServerContext::FinalizeInitialization() {
  std::unique_lock lock(m_stateMutex);
  m_state = ServerState::Initialized;
}

void ServerContext::Terminate() {
  std::unique_lock lock(m_stateMutex);

  m_state = ServerState::Terminated;
  m_lobby = nullptr;
  m_broadcaster = nullptr;
  m_cachedEntrants.clear();
  m_serverDbPeer = EchoVR::TcpPeer_InvalidPeer;

  {
    std::lock_guard sessionLock(m_sessionMutex);
    m_sessionState.Reset();
  }

  m_callbacks.Clear();
}

bool ServerContext::SetRegistered(bool registered) {
  std::unique_lock lock(m_stateMutex);

  if (m_state == ServerState::Uninitialized || m_state == ServerState::Terminated) {
    return false;
  }

  m_state = registered ? ServerState::Registered : ServerState::Initialized;
  return true;
}

bool ServerContext::StartSession() {
  std::unique_lock lock(m_stateMutex);

  if (m_state != ServerState::Registered) {
    return false;
  }

  m_state = ServerState::InSession;

  {
    std::lock_guard sessionLock(m_sessionMutex);
    m_sessionState.active = true;
  }

  return true;
}

bool ServerContext::EndSession() {
  std::unique_lock lock(m_stateMutex);

  if (m_state != ServerState::InSession) {
    return false;
  }

  m_state = ServerState::Registered;

  {
    std::lock_guard sessionLock(m_sessionMutex);
    m_sessionState.active = false;
    m_sessionState.lobbySessionId.clear();
  }

  return true;
}

ServerState ServerContext::GetState() const {
  std::shared_lock lock(m_stateMutex);
  return m_state;
}

bool ServerContext::IsInitialized() const {
  std::shared_lock lock(m_stateMutex);
  return m_state != ServerState::Uninitialized && m_state != ServerState::Terminated;
}

bool ServerContext::IsRegistered() const {
  std::shared_lock lock(m_stateMutex);
  return m_state == ServerState::Registered || m_state == ServerState::InSession;
}

bool ServerContext::IsSessionActive() const {
  std::shared_lock lock(m_stateMutex);
  return m_state == ServerState::InSession;
}

bool ServerContext::IsValidForOperations() const {
  std::shared_lock lock(m_stateMutex);
  return m_state == ServerState::InSession;
}

EchoVR::Lobby* ServerContext::GetLobby() const {
  std::shared_lock lock(m_stateMutex);
  return (m_state != ServerState::Uninitialized && m_state != ServerState::Terminated) ? m_lobby : nullptr;
}

EchoVR::Broadcaster* ServerContext::GetBroadcaster() const {
  std::shared_lock lock(m_stateMutex);
  return (m_state != ServerState::Uninitialized && m_state != ServerState::Terminated) ? m_broadcaster : nullptr;
}

EchoVR::TcpBroadcasterData* ServerContext::GetTcpBroadcaster() const {
  std::shared_lock lock(m_stateMutex);
  if (m_state == ServerState::Uninitialized || m_state == ServerState::Terminated) {
    return nullptr;
  }
  // Dynamically get tcpBroadcaster from lobby instead of caching it
  if (m_lobby && m_lobby->tcpBroadcaster) {
    return m_lobby->tcpBroadcaster->data;
  }
  return nullptr;
}

EchoVR::Lobby::EntrantData* ServerContext::GetEntrant(uint32_t index) const {
  std::shared_lock lock(m_stateMutex);

  if (m_state == ServerState::Uninitialized || m_state == ServerState::Terminated) {
    return nullptr;
  }

  if (index >= m_cachedEntrants.size()) {
    return nullptr;
  }

  return const_cast<EchoVR::Lobby::EntrantData*>(&m_cachedEntrants[index]);
}

uint64_t ServerContext::GetEntrantCount() const {
  std::shared_lock lock(m_stateMutex);

  if (m_state == ServerState::Uninitialized || m_state == ServerState::Terminated) {
    return 0;
  }

  return m_cachedEntrants.size();
}

void ServerContext::SetServerDbPeer(const EchoVR::TcpPeer& peer) {
  std::unique_lock lock(m_stateMutex);
  m_serverDbPeer = peer;
}

EchoVR::TcpPeer ServerContext::GetServerDbPeer() const {
  std::shared_lock lock(m_stateMutex);
  return m_serverDbPeer;
}

SessionState ServerContext::GetSessionState() const {
  std::lock_guard lock(m_sessionMutex);
  return m_sessionState;
}

void ServerContext::UpdateSessionState(const SessionState& state) {
  std::lock_guard lock(m_sessionMutex);
  m_sessionState = state;
}

CallbackRegistry& ServerContext::GetCallbackRegistry() {
  // Not synchronized — only safe from the game's main thread.
  // All current call sites (RegisterBroadcasterCallbacks, UnregisterAllCallbacks,
  // Initialize, Terminate) run on the main thread.
  return m_callbacks;
}

const CallbackRegistry& ServerContext::GetCallbackRegistry() const { return m_callbacks; }

}  // namespace GameServer
