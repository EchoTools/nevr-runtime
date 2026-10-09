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
//
// Every log line is one JSON object with an "event" field (written with nlohmann::json):
//   router_listener  action listening | lost | restored | restore_failed | stopped | start_failed
//   router_game_conn action accepted | rejected | upgraded | upgrade_refused | closing | ended
// A rejected or refused connection carries a fixed "reason" token; an ended one carries the reason, whether
// it was upgraded and how long it lived. The listener is probed every listenerCheckMs: a listening socket
// that was closed underneath this class, replaced by another file at the same number, or is no longer
// listening is reported once (`lost`, with the class that tells those apart) and listened for again on the
// same port, because the game was already handed that port and token.

#include <sys/types.h>

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
    int idleFirstFrameMs = 30000;  // an upgraded connection that sends no data frame in this long is closed
    int closeFlushTimeoutMs = 1000;
    int listenerCheckMs = 2000;  // how often the listening socket is proven to still be ours and listening
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

  // The URI the game must be redirected to: "ws://127.0.0.1:<port>/<token>/". The token is drawn from the
  // kernel's random source at Start(), is never logged, and every upgrade must carry it (path or query) or
  // is answered 403. Empty before a successful Start().
  std::string LoopbackUri() const;
  // Upgrades refused (no/wrong token, Origin header) and connections closed for sending no data frame.
  uint64_t RejectedUpgrades() const { return rejected_.load(); }
  uint64_t IdleClosed() const { return idleClosed_.load(); }
  // Times the listening socket was found lost, and times it was listened for again on the same port.
  uint64_t ListenerLosses() const { return listenerLosses_.load(); }
  uint64_t ListenerRestores() const { return listenerRestores_.load(); }

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
  // Accepts every queued connection. Returns 0, or the errno of an accept() failure that says the listener
  // itself is broken; sets *resourceBlocked when accept() failed for lack of descriptors or memory.
  int AcceptPending(bool* resourceBlocked);
  // Accept thread only. Proves listenFd_ is still the socket Start() made and still listening; on a loss,
  // reports it and drops the descriptor (closing it only when it is still ours).
  void CheckListener(short revents, int acceptErrno);
  // Accept thread only. Listens on port_ again after a loss; logs the outcome on each change.
  void TryRestoreListener();
  // Accept thread only. Cheap identity probe (fstat inode/device) run BEFORE accepting, so a
  // descriptor whose number was reused by another socket is never accepted on. false also when the
  // listener is gone. CheckListener does the full classification and logging.
  bool ListenerStillOurs() const;

  Config config_;
  SessionRouter::Router* router_ = nullptr;
  std::mutex lifecycleMutex_;    // serializes Start() and Stop() so the accept thread is created/joined once
  std::atomic<int> listenFd_{-1};  // written by the accept thread (CheckListener/TryRestoreListener) and by
                                 // Start/Stop around it; atomic so those reads/writes are defined
  ino_t listenIno_ = 0;          // inode and device of the listening socket; a different file at listenFd_ is
  dev_t listenDev_ = 0;          // not ours
  int lastRestoreErrno_ = 0;     // accept thread only: the last restore failure reported, so a retry that fails
                                 // the same way is not logged again
  int lastAcceptErrno_ = 0;      // accept thread only: same, for accept() resource errors
  std::atomic<uint64_t> listenerLosses_{0};
  std::atomic<uint64_t> listenerRestores_{0};
  int wakeFds_[2] = {-1, -1};
  uint16_t port_ = 0;
  std::string token_;  // written once in Start() before any thread runs; read-only afterwards
  std::atomic<uint64_t> rejected_{0};
  std::atomic<uint64_t> idleClosed_{0};
  std::atomic<bool> stop_{false};
  std::thread acceptThread_;
  std::mutex mutex_;  // guards conns_ only
  std::map<SessionRouter::GameId, std::shared_ptr<Conn>> conns_;
  SessionRouter::GameId nextId_ = 1;
};

}  // namespace quest_net
