#pragma once
// The game-facing half of the Quest transport: a WebSocket server on 127.0.0.1 that the game's redirected
// connections reach, implementing SessionRouter::GameTransport. POSIX sockets only (builds for Android
// and for the Linux host, where the tests drive it with real loopback connections).
//
// One thread accepts; one thread per connection reads, so a slow or stuck connection never holds up the
// others. Every Router call and every Log call happens with none of this class's locks held. Send() and
// Close() never block: a send that does not fit the socket buffer is kept (bounded) and flushed by the
// connection's own thread, and the router is told through OnGameWritable when it can send again.
//
// Nothing here logs a payload, a header value or the request target.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "runtime/compat/session_router.h"

namespace quest_net {

class LoopbackGameServer final : public SessionRouter::GameTransport {
 public:
  struct Config {
    std::size_t maxMessageBytes = 4u * 1024u * 1024u;  // one WebSocket message from the game
    std::size_t maxConnections = 16;
    std::size_t maxWriteBufferBytes = 8u * 1024u * 1024u;  // unsent bytes kept per connection
    int handshakeTimeoutMs = 5000;
    int closeFlushTimeoutMs = 1000;
    SessionRouter::LogSink log;
  };

  explicit LoopbackGameServer(Config config);
  ~LoopbackGameServer() override;
  LoopbackGameServer(const LoopbackGameServer&) = delete;
  LoopbackGameServer& operator=(const LoopbackGameServer&) = delete;

  // Must be called before Start. The router must outlive Stop().
  void Attach(SessionRouter::Router* router) { router_ = router; }

  // Binds 127.0.0.1 on an ephemeral port and starts accepting. Returns the port, or 0 on failure (logged).
  uint16_t Start();
  // Closes the listener and every connection, joins every thread. Each connection that completed its
  // handshake reports OnGameClose to the router before this returns. Idempotent.
  void Stop();
  uint16_t port() const { return port_; }

  // SessionRouter::GameTransport
  SessionRouter::SendResult Send(SessionRouter::GameId game, std::string_view frame, bool binary) override;
  void Close(SessionRouter::GameId game, uint16_t code, std::string_view reason) override;

 private:
  struct Conn;
  void AcceptLoop();
  void ConnLoop(std::shared_ptr<Conn> conn);
  std::shared_ptr<Conn> Find(SessionRouter::GameId game);
  void Reap();
  void Log(SessionRouter::LogLevel level, const std::string& line);

  Config config_;
  SessionRouter::Router* router_ = nullptr;
  int listenFd_ = -1;
  int wakeFds_[2] = {-1, -1};
  uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread acceptThread_;
  std::mutex mutex_;  // guards conns_ only
  std::map<SessionRouter::GameId, std::shared_ptr<Conn>> conns_;
  SessionRouter::GameId nextId_ = 1;
};

}  // namespace quest_net
