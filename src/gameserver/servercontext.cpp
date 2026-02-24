#include "servercontext.h"

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
  serverId = 0;
  regionId = 0;
  versionLock = 0;
  gameServerAddr = {};
  defaultTimeStepUsecs = 0;
}

// ServerContext implementation
void ServerContext::Initialize(EchoVR::Lobby* lobby, EchoVR::Broadcaster* broadcaster) {
  std::unique_lock lock(stateMutex_);

  if (lobby && lobby->entrantData.items) {
    cachedEntrants_.clear();
    cachedEntrants_.reserve(lobby->entrantData.count);
    for (uint64_t i = 0; i < lobby->entrantData.count; ++i) {
      cachedEntrants_.push_back(lobby->entrantData.items[i]);
    }
  }

  lobby_ = lobby;
  broadcaster_ = broadcaster;

  serverDbPeer_ = EchoVR::TcpPeer_InvalidPeer;

  {
    std::lock_guard sessionLock(sessionMutex_);
    sessionState_.Reset();
  }

  callbacks_.Clear();
}

void ServerContext::FinalizeInitialization() {
  std::unique_lock lock(stateMutex_);
  state_ = ServerState::Initialized;
}

void ServerContext::Terminate() {
  std::unique_lock lock(stateMutex_);

  state_ = ServerState::Terminated;
  lobby_ = nullptr;
  broadcaster_ = nullptr;
  cachedEntrants_.clear();
  serverDbPeer_ = EchoVR::TcpPeer_InvalidPeer;

  {
    std::lock_guard sessionLock(sessionMutex_);
    sessionState_.Reset();
  }

  callbacks_.Clear();
}

bool ServerContext::SetRegistered(bool registered) {
  std::unique_lock lock(stateMutex_);

  if (state_ == ServerState::Uninitialized || state_ == ServerState::Terminated) {
    return false;
  }

  state_ = registered ? ServerState::Registered : ServerState::Initialized;
  return true;
}

bool ServerContext::StartSession() {
  std::unique_lock lock(stateMutex_);

  if (state_ != ServerState::Registered) {
    return false;
  }

  state_ = ServerState::InSession;

  {
    std::lock_guard sessionLock(sessionMutex_);
    sessionState_.active = true;
  }

  return true;
}

bool ServerContext::EndSession() {
  std::unique_lock lock(stateMutex_);

  if (state_ != ServerState::InSession) {
    return false;
  }

  state_ = ServerState::Registered;

  {
    std::lock_guard sessionLock(sessionMutex_);
    sessionState_.active = false;
  }

  return true;
}

ServerState ServerContext::GetState() const {
  std::shared_lock lock(stateMutex_);
  return state_;
}

bool ServerContext::IsInitialized() const {
  std::shared_lock lock(stateMutex_);
  return state_ != ServerState::Uninitialized && state_ != ServerState::Terminated;
}

bool ServerContext::IsRegistered() const {
  std::shared_lock lock(stateMutex_);
  return state_ == ServerState::Registered || state_ == ServerState::InSession;
}

bool ServerContext::IsSessionActive() const {
  std::shared_lock lock(stateMutex_);
  return state_ == ServerState::InSession;
}

bool ServerContext::IsValidForOperations() const {
  std::shared_lock lock(stateMutex_);
  return state_ == ServerState::InSession;
}

EchoVR::Lobby* ServerContext::GetLobby() const {
  std::shared_lock lock(stateMutex_);
  return (state_ != ServerState::Uninitialized && state_ != ServerState::Terminated) ? lobby_ : nullptr;
}

EchoVR::Broadcaster* ServerContext::GetBroadcaster() const {
  std::shared_lock lock(stateMutex_);
  return (state_ != ServerState::Uninitialized && state_ != ServerState::Terminated) ? broadcaster_ : nullptr;
}

EchoVR::TcpBroadcasterData* ServerContext::GetTcpBroadcaster() const {
  std::shared_lock lock(stateMutex_);
  if (state_ == ServerState::Uninitialized || state_ == ServerState::Terminated) {
    return nullptr;
  }
  // Dynamically get tcpBroadcaster from lobby instead of caching it
  if (lobby_ && lobby_->tcpBroadcaster) {
    return lobby_->tcpBroadcaster->data;
  }
  return nullptr;
}

EchoVR::Lobby::EntrantData* ServerContext::GetEntrant(uint32_t index) const {
  std::shared_lock lock(stateMutex_);

  if (state_ == ServerState::Uninitialized || state_ == ServerState::Terminated) {
    return nullptr;
  }

  if (index >= cachedEntrants_.size()) {
    return nullptr;
  }

  return const_cast<EchoVR::Lobby::EntrantData*>(&cachedEntrants_[index]);
}

uint64_t ServerContext::GetEntrantCount() const {
  std::shared_lock lock(stateMutex_);

  if (state_ == ServerState::Uninitialized || state_ == ServerState::Terminated) {
    return 0;
  }

  return cachedEntrants_.size();
}

void ServerContext::SetServerDbPeer(const EchoVR::TcpPeer& peer) {
  std::unique_lock lock(stateMutex_);
  serverDbPeer_ = peer;
}

EchoVR::TcpPeer ServerContext::GetServerDbPeer() const {
  std::shared_lock lock(stateMutex_);
  return serverDbPeer_;
}

SessionState ServerContext::GetSessionState() const {
  std::lock_guard lock(sessionMutex_);
  return sessionState_;
}

void ServerContext::UpdateSessionState(const SessionState& state) {
  std::lock_guard lock(sessionMutex_);
  sessionState_ = state;
}

CallbackRegistry& ServerContext::GetCallbackRegistry() {
  // No lock needed - caller should hold appropriate lock if concurrent access
  return callbacks_;
}

const CallbackRegistry& ServerContext::GetCallbackRegistry() const { return callbacks_; }

}  // namespace GameServer
