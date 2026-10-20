#pragma once
// ixwebsocket adapters for nevr_session_router on the PC bridge (#708): the two transports the router calls out
// through, and the maps from the router's ids to live sockets. The router owns the routing rules; this file owns
// threads and sockets.
//
// Threads and locks. ixwebsocket runs each socket's callback on that socket's own thread. Two hazards follow,
// and both are handled here, not in the router:
//   * Closing a socket from its own callback thread (or from a thread that holds a lock a callback waits for)
//     joins itself or deadlocks. Every Close/stop the router asks for runs on a detached thread that holds the
//     socket's shared_ptr and no lock (the #70 pattern of ws_bridge.cpp).
//   * One mutex guards each adapter's tables. It is never held across a call into the router, a socket send or
//     a socket stop; the router never holds its own mutex across a call into an adapter.
// A remote's socket is destroyed on a thread that is not its own callback thread.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>

#include "runtime/compat/session_router.h"

namespace nevr_ws_transport {

using nevr_session_router::GameId;
using nevr_session_router::RemoteId;

// Finds the live shared_ptr of a game-side socket from its address (the servers' client sets own them), or null
// when the socket is gone.
using GameSocketLookup = std::function<std::shared_ptr<ix::WebSocket>(const ix::WebSocket*)>;

class GameSockets final : public nevr_session_router::GameTransport {
 public:
  explicit GameSockets(GameSocketLookup lookup);
  // Names a newly accepted socket; the id is never reused. `connIdx` is the bridge's own slot number, kept for logs.
  GameId Register(const ix::WebSocket* socket, int connIdx);
  std::optional<GameId> IdOf(const ix::WebSocket* socket) const;
  int ConnIdxOf(const ix::WebSocket* socket) const;  // -1 when unknown
  void Forget(const ix::WebSocket* socket);
  // A rewrite applied to every frame sent to a game (returns the replacement, or nothing to send it unchanged).
  // Set once, before the first Send.
  using OutboundRewrite = std::function<std::optional<std::string>(const std::string& frame, bool binary)>;
  void SetOutboundRewrite(OutboundRewrite rewrite) { rewrite_ = std::move(rewrite); }

  nevr_session_router::SendResult Send(GameId game, std::string_view frame, bool binary) override;
  void Close(GameId game, uint16_t code, std::string_view reason) override;

 private:
  struct Entry {
    const ix::WebSocket* socket = nullptr;
    int connIdx = -1;
  };
  GameSocketLookup lookup_;
  OutboundRewrite rewrite_;
  mutable std::mutex mutex_;
  std::unordered_map<GameId, Entry> byId_;
  std::unordered_map<const ix::WebSocket*, GameId> bySocket_;
  GameId next_ = 1;
};

struct RemoteHooks {
  // Builds the remote for `request` (URL, credentials, headers) and returns it unstarted; null when it cannot be
  // built. Runs on the thread that opened the connection, with no adapter lock held.
  std::function<std::shared_ptr<ix::WebSocket>(const nevr_session_router::RemoteOpenRequest&)> prepare;
  // Sees every frame a remote delivers before the router routes it (the bridge's logs and observers).
  std::function<void(const nevr_session_router::RemoteOpenRequest&, const std::string& frame)> tap;
  // Wraps a socket callback in the bridge's exception boundary (N85).
  std::function<std::function<void(const ix::WebSocketMessagePtr&)>(std::function<void(const ix::WebSocketMessagePtr&)>)>
      guard;
  std::function<void(nevr_session_router::LogLevel, const std::string&)> log;
};

class RemoteSockets final : public nevr_session_router::RemoteTransport {
 public:
  explicit RemoteSockets(RemoteHooks hooks);
  ~RemoteSockets() override = default;
  // Where socket events go. Set once, before the first Open.
  void SetRouter(nevr_session_router::Router* router) { router_ = router; }

  bool Open(const nevr_session_router::RemoteOpenRequest& request) override;
  nevr_session_router::SendResult Send(RemoteId remote, std::string_view frame, bool binary) override;
  void Close(RemoteId remote, uint16_t code) override;

  // Stops every remote on the calling thread and refuses further Opens: the graceful shutdown path (N38/N64/N105).
  // Returns the number of sockets stopped.
  std::size_t StopAll();

 private:
  struct Entry {
    std::shared_ptr<ix::WebSocket> socket;
    nevr_session_router::RemoteOpenRequest request;
  };
  void OnEvent(RemoteId remote, const ix::WebSocketMessagePtr& message);
  void Retire(RemoteId remote);

  RemoteHooks hooks_;
  nevr_session_router::Router* router_ = nullptr;
  std::mutex mutex_;
  std::unordered_map<RemoteId, Entry> live_;
  std::vector<std::shared_ptr<ix::WebSocket>> retired_;  // ended remotes, destroyed by the next Open
  bool stopped_ = false;
};

}  // namespace nevr_ws_transport
