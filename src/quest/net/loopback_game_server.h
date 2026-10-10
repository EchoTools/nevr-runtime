#pragma once
// The game-facing half of the Quest transport: a WebSocket server on kListenAddress (127.0.0.2) that the
// game's redirected connections reach, implementing nevr_session_router::GameTransport. POSIX sockets only
// (builds for Android and for the Linux host, where the tests drive it with real loopback connections).
//
// Why not 127.0.0.1: the game never dials 127.0.0.1. Its TCP peers resolve their host through
// NRadEngine::CSysNet::Lookup(char const*, unsigned short) (libr15.so 0xf991a4, reached from
// SClientData::STcpPeerData::Connect 0x24fd120 via CDnsLookup::Lookup), which compares the host against
// "localhost" and "127.0.0.1" (CSysString::Compare, whole string, ASCII case-insensitive) and, on a match
// or an empty host, calls getifaddrs and dials the first running non-loopback IPv4 interface instead
// (the Quest's wlan0), or, when none is running, the first non-loopback one. Any other host goes to
// getaddrinfo(host, "%hu") and is dialled as given. libpnsrad.so, libpnsradmatchmaking.so and
// libpnsovr.so link their own copies of CSysNet::Lookup with the same rule. Every other 127/8 address
// is local on Linux and Android ("local 127.0.0.0/8 dev lo"), so 127.0.0.2 keeps the listener
// loopback-only and is dialled verbatim. GameDialsHostVerbatim encodes that rule.
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
//   router_game_conn action accepted | rejected | upgraded | upgrade_refused | closing | idle_exempt | idle_enforced | ended
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

// The address the listener binds and LoopbackUri() names. See the note at the top of this file.
inline constexpr char kListenAddress[] = "127.0.0.2";

namespace detail {
constexpr char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 0x20) : c; }
constexpr bool EqualsIgnoreAsciiCase(const char* a, const char* b) {
  for (;; ++a, ++b) {
    if (AsciiLower(*a) != AsciiLower(*b)) return false;
    if (*a == '\0') return true;
  }
}
}  // namespace detail

// Whether libr15's CSysNet::Lookup dials `host` as written. False for an empty host and for "localhost"
// and "127.0.0.1" in any ASCII case: those it replaces with a non-loopback interface address.
constexpr bool GameDialsHostVerbatim(const char* host) {
  return host != nullptr && *host != '\0' && !detail::EqualsIgnoreAsciiCase(host, "localhost") &&
         !detail::EqualsIgnoreAsciiCase(host, "127.0.0.1");
}
static_assert(GameDialsHostVerbatim(kListenAddress), "the game would not dial the listener's address");

class LoopbackGameServer final : public nevr_session_router::GameTransport {
 public:
  struct Config {
    std::size_t maxMessageBytes = 4u * 1024u * 1024u;  // one WebSocket message from the game
    std::size_t maxConnections = 16;
    std::size_t maxWriteBufferBytes = 8u * 1024u * 1024u;  // unsent bytes kept per connection
    int handshakeTimeoutMs = 5000;
    int idleFirstFrameMs = 30000;  // an upgraded connection that sends no data frame in this long is closed
    int closeFlushTimeoutMs = 1000;
    int listenerCheckMs = 2000;  // how often the listening socket is proven to still be ours and listening
    nevr_session_router::LogSink log;
  };

  explicit LoopbackGameServer(Config config);
  ~LoopbackGameServer() override;
  LoopbackGameServer(const LoopbackGameServer&) = delete;
  LoopbackGameServer& operator=(const LoopbackGameServer&) = delete;

  // Must be called before Start. The router must outlive Stop().
  void Attach(nevr_session_router::Router* router) { router_ = router; }

  // Binds kListenAddress on an ephemeral port and starts accepting. Returns the port, or 0 on failure (logged).
  // Start() and Stop() are the startup owner's to call; they must NOT be called from a router callback
  // or from a connection thread, because Stop() joins those threads while it holds lifecycleMutex_ (a
  // thread that called Stop() on itself, or a Start() racing that join, would deadlock). The transport
  // interface the router drives (Send/Close) never calls them.
  uint16_t Start();
  // Closes the listener and every connection, joins every thread. Each connection that completed its
  // handshake reports OnGameClose to the router before this returns. Idempotent. See the thread note above.
  void Stop();
  uint16_t port() const { return port_.load(std::memory_order_acquire); }

  // The URI the game must be redirected to: "ws://127.0.0.2:<port>/<token>/" (kListenAddress). The token is drawn from the
  // kernel's random source at Start(), is never logged, and every upgrade must carry it (path or query) or
  // is answered 403. Empty before a successful Start().
  std::string LoopbackUri() const;
  // Upgrades refused (no/wrong token, Origin header) and connections closed for sending no data frame.
  uint64_t RejectedUpgrades() const { return rejected_.load(); }
  uint64_t IdleClosed() const { return idleClosed_.load(); }
  // Times the listening socket was found lost, and times it was listened for again on the same port.
  uint64_t ListenerLosses() const { return listenerLosses_.load(); }
  uint64_t ListenerRestores() const { return listenerRestores_.load(); }

  // nevr_session_router::GameTransport
  nevr_session_router::SendResult Send(nevr_session_router::GameId game, std::string_view frame, bool binary) override;
  void Close(nevr_session_router::GameId game, uint16_t code, std::string_view reason) override;
  // The login connection is silent until the game's LogInRequest, which can be minutes after it connects (the
  // player has to sign in first), so it is exempt from the idle-before-first-frame close for as long as it is
  // the login connection. It is still answered to pings and closed by the router, the peer or Stop().
  void SetIdleExempt(nevr_session_router::GameId game, bool exempt) override;

 private:
  struct Conn;
  void AcceptLoop();
  void ConnLoop(std::shared_ptr<Conn> conn);
  std::shared_ptr<Conn> Find(nevr_session_router::GameId game);
  void Reap();
  void Log(nevr_session_router::LogLevel level, const std::string& line);
  // Accepts every queued connection. Returns 0, or the errno of an accept() failure that says the listener
  // itself is broken; sets *resourceBlocked when accept() failed for lack of descriptors or memory, and
  // *identityLost when the descriptor stopped being our listening socket mid-drain (re-checked per accept).
  int AcceptPending(bool* resourceBlocked, bool* identityLost);
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
  nevr_session_router::Router* router_ = nullptr;
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
  std::atomic<uint16_t> port_{0};  // set by Start, cleared by Stop; read unlocked by port()
  mutable std::mutex uriMutex_;    // guards token_ and the (port, token) pair LoopbackUri() reads
  std::string token_;              // set by Start, cleared by Stop, under uriMutex_
  std::atomic<uint64_t> rejected_{0};
  std::atomic<uint64_t> idleClosed_{0};
  std::atomic<bool> stop_{false};
  std::thread acceptThread_;
  std::mutex mutex_;  // guards conns_ only
  std::map<nevr_session_router::GameId, std::shared_ptr<Conn>> conns_;
  nevr_session_router::GameId nextId_ = 1;
};

}  // namespace quest_net
