#pragma once

// #1756: a dedicated server whose session the service ended has to leave. The normal exit is the game's own:
// ServerDB's CODE_ENDED makes the runtime ask the game to return to lobby, the game leaves the session, the state
// machine sees InGame -> Lobby and exits the process (lifecycle/state_machine.cpp). Four Combat matches on 6.12.0
// ended on the service, got CODE_ENDED, and their server never left. Nothing on the runtime side checked that the
// return happened, so the process stayed up and kept its slot.
//
// This is the check. The ServerDB receive thread arms it the moment a CODE_ENDED frame arrives, before the frame
// is queued for the game thread (a game thread that has stopped calling Update() would never get to it). If the
// process is still alive when the delay runs out, `onExpire` ends it. Pure of game memory: the delay, the exit
// action and the clock are the caller's, so a host test drives it with a short delay and a recording action.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/protobuf_transport.h"

namespace nevr_game_server {

/// True when a ServerDB message is a protobuf Envelope carrying LobbySessionEvent CODE_ENDED: the service has ended
/// the session and the server is done. `sessionId` receives the session the event names.
inline bool ParseServiceEndedSession(EchoVR::SymbolId msgId, const void* data, size_t size, std::string* sessionId) {
  if (msgId != kProtobufMessageSymbol || data == nullptr || size == 0) return false;
  gameservice::v1::Envelope envelope;
  if (!envelope.ParseFromArray(data, static_cast<int>(size))) return false;
  if (envelope.message_case() != gameservice::v1::Envelope::kLobbySessionEvent) return false;
  const auto& event = envelope.lobby_session_event();
  if (event.code() != gameservice::v1::LobbySessionEventMessage::CODE_ENDED) return false;
  if (sessionId != nullptr) *sessionId = event.lobby_session_id();
  return true;
}

class EndedSessionWatchdog {
 public:
  using ExpireFn = std::function<void(const std::string& sessionId)>;

  EndedSessionWatchdog() = default;
  EndedSessionWatchdog(const EndedSessionWatchdog&) = delete;
  EndedSessionWatchdog& operator=(const EndedSessionWatchdog&) = delete;
  ~EndedSessionWatchdog() { Cancel(); }

  /// Starts the countdown. Returns false, and changes nothing, when it is already running or has already fired
  /// (a second CODE_ENDED must not push the deadline back).
  bool Arm(std::chrono::milliseconds delay, std::string sessionId, ExpireFn onExpire) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (armed_ || fired_) return false;
    armed_ = true;
    const uint64_t generation = ++generation_;
    thread_ = std::thread([this, generation, delay, id = std::move(sessionId), fn = std::move(onExpire)]() {
      std::unique_lock<std::mutex> wait(mutex_);
      // A Cancel() followed by a new Arm() leaves armed_ true again; the generation tells this countdown it was
      // the one cancelled.
      const bool cancelled = cv_.wait_for(wait, delay, [this, generation] { return !armed_ || generation_ != generation; });
      if (cancelled || !armed_ || generation_ != generation) return;
      armed_ = false;
      fired_ = true;
      wait.unlock();
      if (fn) fn(id);
    });
    return true;
  }

  /// Stops a running countdown. Safe from any thread, and from inside `onExpire`.
  void Cancel() {
    std::thread stopped;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      armed_ = false;
      ++generation_;
      stopped = std::move(thread_);
    }
    cv_.notify_all();
    if (!stopped.joinable()) return;
    if (stopped.get_id() == std::this_thread::get_id()) {
      stopped.detach();  // called from inside onExpire: this thread cannot join itself
    } else {
      stopped.join();
    }
  }

  bool Armed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return armed_;
  }

  bool Fired() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fired_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool armed_ = false;
  bool fired_ = false;
  uint64_t generation_ = 0;
  std::thread thread_;
};

}  // namespace nevr_game_server
