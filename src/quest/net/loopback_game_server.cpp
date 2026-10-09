#include "quest/net/loopback_game_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/net/ws_wire.h"

namespace quest_net {

using SessionRouter::GameId;
using SessionRouter::LogLevel;
using SessionRouter::SendResult;

namespace {

constexpr const char* kListenerEvent = "router_listener";
constexpr const char* kConnEvent = "router_game_conn";

nlohmann::json Record(const char* event, const char* action) {
  nlohmann::json record;
  record["event"] = event;
  record["action"] = action;
  return record;
}

// One JSON line. Every value written here is a number or a fixed ASCII token, so dump() cannot meet invalid
// UTF-8; the fallback is still a valid record rather than a lost one.
std::string Dump(const nlohmann::json& record) {
  try {
    return record.dump();
  } catch (const std::exception&) {
    return "{\"event\":\"router_log_error\"}";
  }
}

long long MillisSince(std::chrono::steady_clock::time_point start) {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

// A non-blocking, close-on-exec TCP listener on kListenAddress:`port` (0 = an ephemeral port). Returns the
// descriptor and fills *st with its identity, or -1 with *err set.
int OpenListener(uint16_t port, struct stat* st, uint16_t* boundPort, int* err) {
  in_addr listenAddr{};
  if (::inet_pton(AF_INET, kListenAddress, &listenAddr) != 1) {
    *err = EINVAL;
    return -1;
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    *err = errno;
    return -1;
  }
  if (port != 0) {
    // Listening again on the port the game already holds: connections that ended moments ago may sit in
    // TIME_WAIT on it.
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  }
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr = listenAddr;
  addr.sin_port = htons(port);
  socklen_t len = sizeof(addr);
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0 ||
      ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 || flags < 0 ||
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 || ::fstat(fd, st) != 0) {
    *err = errno;
    ::close(fd);
    return -1;
  }
  *boundPort = ntohs(addr.sin_port);
  return fd;
}

bool SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void MakePipe(int fds[2]) {
  if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
    fds[0] = -1;
    fds[1] = -1;
  }
}

void CloseFd(int& fd) {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

// 16 bytes from the kernel's CSPRNG. False (and nothing usable) on any failure: no token, no listener.
bool RandomBytes(uint8_t* out, std::size_t count) {
  std::size_t have = 0;
  while (have < count) {
    const long n = ::syscall(SYS_getrandom, out + have, count - have, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    have += static_cast<std::size_t>(n);
  }
  return true;
}

void Poke(int fd) {
  const char byte = 1;
  if (fd >= 0) {
    const ssize_t n = ::write(fd, &byte, 1);
    (void)n;  // a full pipe already holds a wakeup
  }
}

void Drain(int fd) {
  char buf[64];
  while (fd >= 0 && ::read(fd, buf, sizeof(buf)) > 0) {
  }
}

}  // namespace

struct LoopbackGameServer::Conn {
  GameId id = 0;
  int fd = -1;
  int wake[2] = {-1, -1};
  std::thread thread;
  std::atomic<bool> done{false};

  std::mutex writeMutex;  // guards writeBuf, closeRequested, closeAfterFlush
  std::string writeBuf;
  bool closeRequested = false;
  bool blocked = false;  // Send returned WouldBlock; owe the router an OnGameWritable
  std::atomic<bool> held{false};            // exempt from the idle-before-first-frame close (SetHeld)
  std::atomic<long long> releasedAtNs{0};   // steady-clock ns when the hold last ended; 0 = never held
  std::chrono::steady_clock::time_point closeDeadline;
};

LoopbackGameServer::LoopbackGameServer(Config config) : config_(std::move(config)) {}

LoopbackGameServer::~LoopbackGameServer() { Stop(); }

void LoopbackGameServer::Log(LogLevel level, const std::string& line) {
  if (config_.log) config_.log(level, line);
}

std::string LoopbackGameServer::LoopbackUri() const {
  const std::lock_guard<std::mutex> lock(uriMutex_);
  const uint16_t port = port_.load(std::memory_order_acquire);
  if (port == 0 || token_.empty()) return std::string();
  return std::string("ws://") + kListenAddress + ":" + std::to_string(port) + "/" + token_ + "/";
}

uint16_t LoopbackGameServer::Start() {
  const std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
  if (router_ == nullptr) return 0;
  // Already running (even with no live descriptor: after a failed restore the accept thread keeps
  // retrying with listenFd_ < 0). Re-entry must not reassign acceptThread_ while it is joinable --
  // that calls std::terminate -- so return the current port instead.
  if (acceptThread_.joinable() || listenFd_.load(std::memory_order_acquire) >= 0) return port_.load(std::memory_order_acquire);
  uint8_t raw[kTokenHexLength / 2];
  if (!RandomBytes(raw, sizeof(raw))) {
    nlohmann::json record = Record(kListenerEvent, "start_failed");
    record["reason"] = "random_source_failed";  // no token, so no listener
    Log(LogLevel::Error, Dump(record));
    return 0;
  }
  {
    const std::lock_guard<std::mutex> lock(uriMutex_);
    token_ = HexEncode(raw, sizeof(raw));
  }
  volatile uint8_t* scrub = raw;
  for (std::size_t i = 0; i < sizeof(raw); ++i) scrub[i] = 0;
  struct stat st {};
  int err = 0;
  uint16_t port = 0;
  const int listenFd = OpenListener(/*port=*/0, &st, &port, &err);  // ephemeral: never a fixed port
  if (listenFd < 0) {
    nlohmann::json record = Record(kListenerEvent, "start_failed");
    record["reason"] = "listen_failed";
    record["errno"] = err;
    Log(LogLevel::Error, Dump(record));
    return 0;
  }
  listenFd_.store(listenFd, std::memory_order_release);
  listenIno_ = st.st_ino;
  listenDev_ = st.st_dev;
  port_.store(port, std::memory_order_release);
  MakePipe(wakeFds_);
  stop_ = false;
  acceptThread_ = std::thread([this]() { AcceptLoop(); });
  nlohmann::json record = Record(kListenerEvent, "listening");
  record["address"] = kListenAddress;
  record["port"] = port;
  Log(LogLevel::Info, Dump(record));
  return port;
}

void LoopbackGameServer::Stop() {
  const std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
  if (listenFd_.load(std::memory_order_acquire) < 0 && !acceptThread_.joinable()) return;
  stop_ = true;
  Poke(wakeFds_[1]);
  if (acceptThread_.joinable()) acceptThread_.join();
  // The accept thread is joined, so listenFd_ is ours to read and close without a race.
  std::vector<std::shared_ptr<Conn>> all;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : conns_) all.push_back(entry.second);
    conns_.clear();
  }
  for (auto& conn : all) Poke(conn->wake[1]);
  for (auto& conn : all) {
    if (conn->thread.joinable()) conn->thread.join();
    CloseFd(conn->fd);
    CloseFd(conn->wake[0]);
    CloseFd(conn->wake[1]);
  }
  int listenFd = listenFd_.load(std::memory_order_acquire);
  CloseFd(listenFd);                               // sets the local to -1
  listenFd_.store(-1, std::memory_order_release);  // so a later Start() can run again
  CloseFd(wakeFds_[0]);
  CloseFd(wakeFds_[1]);
  wakeFds_[0] = -1;
  wakeFds_[1] = -1;
  nlohmann::json record = Record(kListenerEvent, "stopped");
  record["port"] = port_.load(std::memory_order_acquire);
  Log(LogLevel::Info, Dump(record));
  port_.store(0, std::memory_order_release);  // a later Start() rebinds; port()/LoopbackUri() read 0 meanwhile
  {
    const std::lock_guard<std::mutex> lock(uriMutex_);
    token_.clear();
  }
}

std::shared_ptr<LoopbackGameServer::Conn> LoopbackGameServer::Find(GameId game) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = conns_.find(game);
  return it == conns_.end() ? nullptr : it->second;
}

void LoopbackGameServer::Reap() {
  std::vector<std::shared_ptr<Conn>> finished;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = conns_.begin(); it != conns_.end();) {
      if (it->second->done) {
        finished.push_back(it->second);
        it = conns_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& conn : finished) {
    if (conn->thread.joinable()) conn->thread.join();
    CloseFd(conn->fd);
    CloseFd(conn->wake[0]);
    CloseFd(conn->wake[1]);
  }
}

void LoopbackGameServer::AcceptLoop() {
  auto lastCheck = std::chrono::steady_clock::now();
  bool resourceBlocked = false;
  while (!stop_) {
    const int lfd = listenFd_.load(std::memory_order_acquire);
    // poll() ignores a negative descriptor, so after a loss only the wake pipe and the timeout run. After
    // accept() ran out of descriptors the listener is watched for errors only: a queued connection would
    // otherwise report POLLIN on every pass and spin this thread.
    const short listenInterest = resourceBlocked ? 0 : POLLIN;
    pollfd fds[2] = {{lfd, listenInterest, 0}, {wakeFds_[0], POLLIN, 0}};
    const int ready = ::poll(fds, 2, 250);
    const int pollErrno = ready < 0 ? errno : 0;
    Reap();
    if (stop_) break;
    const short revents = ready > 0 ? fds[0].revents : 0;
    if (ready > 0 && (fds[1].revents & POLLIN) != 0) Drain(wakeFds_[0]);
    const bool faulted = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
    int acceptErrno = 0;
    bool identityLost = false;
    resourceBlocked = false;
    if (lfd >= 0 && !faulted && (revents & POLLIN) != 0) {
      // Before accepting, confirm the descriptor is still our listening socket. If its number was
      // reused by another socket, accept() here would take THAT socket's connections and answer them
      // 403; CheckListener runs only periodically, so the window would be up to listenerCheckMs. The
      // pre-check closes it: a replaced descriptor is never accepted on, it is handed to CheckListener.
      if (ListenerStillOurs()) {
        acceptErrno = AcceptPending(&resourceBlocked, &identityLost);
      } else {
        identityLost = true;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    // Check the listener on a fault, an accept failure, a replaced descriptor, a poll() error (other
    // than EINTR), or on the periodic timer.
    if (faulted || identityLost || acceptErrno != 0 || (pollErrno != 0 && pollErrno != EINTR) ||
        now - lastCheck >= std::chrono::milliseconds(config_.listenerCheckMs)) {
      lastCheck = now;
      if (listenFd_.load(std::memory_order_acquire) >= 0) {
        CheckListener(revents, acceptErrno);
      } else {
        TryRestoreListener();
      }
    }
  }
}

bool LoopbackGameServer::ListenerStillOurs() const {
  const int fd = listenFd_.load(std::memory_order_acquire);
  if (fd < 0) return false;
  struct stat st {};
  if (::fstat(fd, &st) != 0) return false;
  return S_ISSOCK(st.st_mode) && st.st_ino == listenIno_ && st.st_dev == listenDev_;
}

int LoopbackGameServer::AcceptPending(bool* resourceBlocked, bool* identityLost) {
  for (;;) {
    // Re-prove the listener on every pass, not just before the first accept: its number could be
    // closed and reused by another socket mid-drain, and the next accept4 would then take that
    // socket's connections. A changed identity stops the drain and tells the caller to run
    // CheckListener this pass (listenFd_ now resolves fd_replaced/fd_closed).
    if (!ListenerStillOurs()) {
      *identityLost = true;
      return 0;
    }
    const int listenFd = listenFd_.load(std::memory_order_acquire);
    const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
      const int err = errno;
      if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == ECONNABORTED) return 0;
      if (err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM) {
        // The connection stays queued; the game sees it connected but never answered. Said once per errno.
        *resourceBlocked = true;
        if (err != lastAcceptErrno_) {
          lastAcceptErrno_ = err;
          nlohmann::json record = Record(kConnEvent, "rejected");
          record["reason"] = "accept_out_of_resources";
          record["errno"] = err;
          Log(LogLevel::Error, Dump(record));
        }
        return 0;
      }
      return err;  // the listener itself: CheckListener says what happened to it
    }
    lastAcceptErrno_ = 0;
    std::size_t count = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      count = conns_.size();
    }
    const char* refusal = nullptr;
    if (count >= config_.maxConnections) {
      refusal = "connection_limit";
    } else if (!SetNonBlocking(fd)) {
      refusal = "set_nonblocking_failed";
    }
    if (refusal != nullptr) {
      nlohmann::json record = Record(kConnEvent, "rejected");
      record["reason"] = refusal;
      record["open"] = count;
      record["limit"] = config_.maxConnections;
      Log(LogLevel::Warning, Dump(record));
      ::close(fd);
      continue;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    auto conn = std::make_shared<Conn>();
    conn->fd = fd;
    MakePipe(conn->wake);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      conn->id = nextId_++;
      conns_[conn->id] = conn;
    }
    nlohmann::json record = Record(kConnEvent, "accepted");
    record["conn"] = conn->id;
    record["open"] = count + 1;
    Log(LogLevel::Info, Dump(record));
    conn->thread = std::thread([this, conn]() { ConnLoop(conn); });
  }
}

void LoopbackGameServer::CheckListener(short revents, int acceptErrno) {
  const int listenFd = listenFd_.load(std::memory_order_acquire);
  const char* cls = nullptr;
  bool ours = false;
  int soError = 0;
  struct stat st {};
  if (::fstat(listenFd, &st) != 0) {
    cls = "fd_closed";  // something in the process closed the descriptor underneath this class
  } else if (!S_ISSOCK(st.st_mode) || st.st_ino != listenIno_ || st.st_dev != listenDev_) {
    cls = "fd_replaced";  // closed, and the number reused by another file: that file is not ours to touch
  } else {
    ours = true;
    int accepting = 0;
    socklen_t len = sizeof(accepting);
    if (::getsockopt(listenFd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &len) != 0 || accepting == 0) {
      cls = "not_listening";  // still our socket, but the kernel no longer listens on it (shut down or destroyed)
      socklen_t errLen = sizeof(soError);
      if (::getsockopt(listenFd, SOL_SOCKET, SO_ERROR, &soError, &errLen) != 0) soError = 0;
    }
  }
  if (cls == nullptr) return;
  listenerLosses_.fetch_add(1);
  nlohmann::json record = Record(kListenerEvent, "lost");
  record["class"] = cls;
  record["port"] = port_.load(std::memory_order_acquire);
  record["fd"] = listenFd;
  record["revents"] = static_cast<int>(revents);
  record["accept_errno"] = acceptErrno;
  record["so_error"] = soError;
  Log(LogLevel::Error, Dump(record));
  if (ours) ::close(listenFd);
  listenFd_.store(-1, std::memory_order_release);
  lastRestoreErrno_ = 0;
  TryRestoreListener();
}

void LoopbackGameServer::TryRestoreListener() {
  struct stat st {};
  int err = 0;
  uint16_t port = 0;
  const int fd = OpenListener(port_.load(std::memory_order_acquire), &st, &port, &err);
  if (fd < 0) {
    if (err != lastRestoreErrno_) {
      lastRestoreErrno_ = err;
      nlohmann::json record = Record(kListenerEvent, "restore_failed");
      record["port"] = port_.load(std::memory_order_acquire);
      record["errno"] = err;
      record["retry_ms"] = config_.listenerCheckMs;
      Log(LogLevel::Error, Dump(record));
    }
    return;
  }
  listenIno_ = st.st_ino;
  listenDev_ = st.st_dev;
  listenFd_.store(fd, std::memory_order_release);
  lastRestoreErrno_ = 0;
  listenerRestores_.fetch_add(1);
  nlohmann::json record = Record(kListenerEvent, "restored");
  record["port"] = port;
  record["fd"] = fd;
  Log(LogLevel::Warning, Dump(record));
}

namespace {

// Writes as much of `buf` as the socket takes right now. Returns false when the connection is broken.
bool FlushSome(int fd, std::string& buf) {
  while (!buf.empty()) {
    const ssize_t n = ::send(fd, buf.data(), buf.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      buf.erase(0, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

}  // namespace

void LoopbackGameServer::ConnLoop(std::shared_ptr<Conn> conn) {
  const GameId id = conn->id;
  const auto started = std::chrono::steady_clock::now();
  bool opened = false;
  bool open = true;
  const char* endReason = nullptr;  // a fixed token; set where the loop decides to end
  std::string inbox;               // handshake bytes
  FrameDecoder decoder(config_.maxMessageBytes);
  bool handshaken = false;
  bool sawDataFrame = false;
  std::chrono::steady_clock::time_point upgradedAt;

  auto queueWrite = [&](const std::string& bytes, bool bypassCap) {
    std::lock_guard<std::mutex> lock(conn->writeMutex);
    if (!bypassCap && conn->writeBuf.size() + bytes.size() > config_.maxWriteBufferBytes) return false;
    conn->writeBuf += bytes;
    return true;
  };
  auto beginClose = [&](uint16_t code, const char* reason) {
    std::lock_guard<std::mutex> lock(conn->writeMutex);
    if (conn->closeRequested) return;
    conn->closeRequested = true;
    conn->closeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.closeFlushTimeoutMs);
    if (handshaken) conn->writeBuf += BuildCloseFrame(code, reason);
  };

  while (open && !stop_) {
    bool wantWrite = false;
    bool closing = false;
    {
      std::lock_guard<std::mutex> lock(conn->writeMutex);
      wantWrite = !conn->writeBuf.empty();
      closing = conn->closeRequested;
      if (closing && !wantWrite) {  // close frame flushed
        if (endReason == nullptr) endReason = "closed_by_router";
        break;
      }
      if (closing && std::chrono::steady_clock::now() > conn->closeDeadline) {
        endReason = "close_flush_timed_out";
        break;
      }
    }
    if (!handshaken && std::chrono::steady_clock::now() - started > std::chrono::milliseconds(config_.handshakeTimeoutMs)) {
      endReason = "handshake_timed_out";
      break;
    }
    // The idle clock starts at the upgrade, or when a hold ended if that was later.
    auto idleSince = upgradedAt;
    const long long releasedNs = conn->releasedAtNs.load();
    if (releasedNs != 0) {
      const auto released = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(releasedNs));
      if (released > idleSince) idleSince = released;
    }
    if (handshaken && !sawDataFrame && !closing && !conn->held.load() &&
        std::chrono::steady_clock::now() - idleSince > std::chrono::milliseconds(config_.idleFirstFrameMs)) {
      ++idleClosed_;
      nlohmann::json record = Record(kConnEvent, "closing");
      record["conn"] = id;
      record["reason"] = "idle_before_first_frame";
      record["idle_ms"] = config_.idleFirstFrameMs;
      Log(LogLevel::Warning, Dump(record));
      beginClose(SessionRouter::kClosePolicyViolation, "idle");
      endReason = "idle_before_first_frame";
    }
    const short interest = static_cast<short>((closing ? 0 : POLLIN) | (wantWrite ? POLLOUT : 0));
    pollfd fds[2] = {{conn->fd, interest, 0}, {conn->wake[0], POLLIN, 0}};
    const int ready = ::poll(fds, 2, 200);
    if (ready < 0 && errno != EINTR) {
      endReason = "poll_failed";
      break;
    }
    if (ready <= 0) continue;
    if ((fds[1].revents & POLLIN) != 0) Drain(conn->wake[0]);

    if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0) {
      endReason = "socket_error";
      break;
    }
    if (wantWrite && (fds[0].revents & POLLOUT) != 0) {
      bool ok = true;
      bool owe = false;
      {
        std::lock_guard<std::mutex> lock(conn->writeMutex);
        ok = FlushSome(conn->fd, conn->writeBuf);
        if (ok && conn->blocked && conn->writeBuf.size() < config_.maxWriteBufferBytes / 2) {
          conn->blocked = false;
          owe = true;
        }
      }
      if (!ok) {
        endReason = "write_failed";
        break;
      }
      if (owe && opened) router_->OnGameWritable(id);
    }
    if (closing) continue;
    if ((fds[0].revents & (POLLIN | POLLHUP)) == 0) continue;

    char chunk[16384];
    const ssize_t got = ::recv(conn->fd, chunk, sizeof(chunk), MSG_DONTWAIT);
    if (got == 0) {
      endReason = "peer_closed";
      break;
    }
    if (got < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      endReason = "read_failed";
      break;
    }

    if (!handshaken) {
      inbox.append(chunk, static_cast<std::size_t>(got));
      UpgradeRequest request;
      const HandshakeStatus hs = ParseUpgradeRequest(inbox, &request);
      if (hs == HandshakeStatus::NeedMore) continue;
      if (hs != HandshakeStatus::Ok) {
        // Not an upgrade we can answer: say so once and hang up. The request itself is never logged.
        queueWrite(BuildBadRequestResponse(), true);
        beginClose(0, "");
        endReason = hs == HandshakeStatus::TooLarge ? "handshake_too_large" : "bad_handshake";
        nlohmann::json record = Record(kConnEvent, "upgrade_refused");
        record["conn"] = id;
        record["reason"] = endReason;
        record["status"] = 400;
        Log(LogLevel::Warning, Dump(record));
        std::lock_guard<std::mutex> lock(conn->writeMutex);
        FlushSome(conn->fd, conn->writeBuf);
        break;
      }
      // Access check: every local app can reach this port, so the upgrade must carry the token the runtime put
      // in the redirect, and a browser or webview (Origin header) is refused regardless. The token and the
      // request target are never logged.
      const char* refusal = nullptr;
      if (request.hasOrigin) {
        refusal = "origin_header_present";
      } else if (![this, &request] {
                   const std::lock_guard<std::mutex> lock(uriMutex_);
                   return TargetCarriesToken(request.target, token_);
                 }()) {
        refusal = "access_token_missing_or_wrong";
      }
      if (refusal != nullptr) {
        ++rejected_;
        queueWrite(BuildForbiddenResponse(), true);
        beginClose(0, "");
        endReason = "upgrade_refused";
        nlohmann::json record = Record(kConnEvent, "upgrade_refused");
        record["conn"] = id;
        record["reason"] = refusal;
        record["status"] = 403;
        Log(LogLevel::Warning, Dump(record));
        std::lock_guard<std::mutex> lock(conn->writeMutex);
        FlushSome(conn->fd, conn->writeBuf);
        break;
      }
      queueWrite(BuildUpgradeResponse(request.key), true);
      handshaken = true;
      upgradedAt = std::chrono::steady_clock::now();
      {
        std::lock_guard<std::mutex> lock(conn->writeMutex);
        if (!FlushSome(conn->fd, conn->writeBuf)) {
          endReason = "write_failed";
          break;
        }
      }
      opened = true;
      {
        nlohmann::json record = Record(kConnEvent, "upgraded");
        record["conn"] = id;
        record["handshake_ms"] = MillisSince(started);
        Log(LogLevel::Info, Dump(record));
      }
      router_->OnGameOpen(id);
      const std::string leftover = inbox.substr(request.consumed);
      inbox.clear();
      if (!leftover.empty()) decoder.Feed(leftover.data(), leftover.size());
    } else {
      decoder.Feed(chunk, static_cast<std::size_t>(got));
    }

    if (!handshaken) continue;
    for (;;) {
      Message message;
      const DecodeStatus status = decoder.Next(&message);
      if (status == DecodeStatus::NeedMore) break;
      if (status == DecodeStatus::TooBig) {
        nlohmann::json record = Record(kConnEvent, "closing");
        record["conn"] = id;
        record["reason"] = "message_too_big";
        record["limit_bytes"] = config_.maxMessageBytes;
        Log(LogLevel::Error, Dump(record));
        beginClose(SessionRouter::kCloseMessageTooBig, "message too big");
        endReason = "message_too_big";
        break;
      }
      if (status == DecodeStatus::ProtocolError) {
        nlohmann::json record = Record(kConnEvent, "closing");
        record["conn"] = id;
        record["reason"] = "protocol_error";
        Log(LogLevel::Warning, Dump(record));
        beginClose(1002, "protocol error");
        endReason = "protocol_error";
        break;
      }
      switch (message.opcode) {
        case Opcode::Text:
        case Opcode::Binary:
          sawDataFrame = true;
          router_->OnGameFrame(id, std::move(message.payload), message.opcode == Opcode::Binary);
          break;
        case Opcode::Ping:
          queueWrite(BuildFrame(Opcode::Pong, message.payload), true);
          break;
        case Opcode::Close:
          beginClose(message.closeCode == 1005 ? static_cast<uint16_t>(1000) : message.closeCode, "");
          endReason = "peer_sent_close";
          break;
        default:
          break;
      }
    }
    // Opportunistic flush of pongs / close frames queued above.
    {
      std::lock_guard<std::mutex> lock(conn->writeMutex);
      if (!FlushSome(conn->fd, conn->writeBuf)) {
        endReason = "write_failed";
        open = false;
      }
    }
  }

  if (endReason == nullptr) endReason = stop_ ? "server_stopped" : "unknown";
  nlohmann::json record = Record(kConnEvent, "ended");
  record["conn"] = id;
  record["reason"] = endReason;
  record["upgraded"] = opened;
  record["ms"] = MillisSince(started);
  record["unmasked_frames"] = decoder.UnmaskedFrames();  // client frames without the mask bit, accepted
  Log(LogLevel::Info, Dump(record));
  ::shutdown(conn->fd, SHUT_RDWR);
  if (opened) router_->OnGameClose(id);
  conn->done = true;
}

SendResult LoopbackGameServer::Send(GameId game, std::string_view frame, bool binary) {
  const std::shared_ptr<Conn> conn = Find(game);
  if (!conn) return SendResult::Failed;
  const std::string wire = BuildFrame(binary ? Opcode::Binary : Opcode::Text, frame);
  {
    std::lock_guard<std::mutex> lock(conn->writeMutex);
    if (conn->closeRequested) return SendResult::Failed;
    if (conn->writeBuf.size() + wire.size() > config_.maxWriteBufferBytes) {
      conn->blocked = true;
      Poke(conn->wake[1]);
      return SendResult::WouldBlock;
    }
    conn->writeBuf += wire;
    if (!FlushSome(conn->fd, conn->writeBuf)) return SendResult::Failed;
    if (conn->writeBuf.empty()) return SendResult::Sent;
  }
  Poke(conn->wake[1]);  // the connection thread adds POLLOUT and finishes the write
  return SendResult::Sent;
}

void LoopbackGameServer::SetHeld(GameId game, bool held) {
  const std::shared_ptr<Conn> conn = Find(game);
  if (!conn) return;
  if (held) {
    conn->held.store(true);
  } else {
    conn->releasedAtNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
    conn->held.store(false);
  }
  nlohmann::json record = Record(kConnEvent, held ? "held" : "released");
  record["conn"] = game;
  Log(LogLevel::Info, Dump(record));
  Poke(conn->wake[1]);
}

void LoopbackGameServer::Close(GameId game, uint16_t code, std::string_view reason) {
  const std::shared_ptr<Conn> conn = Find(game);
  if (!conn) return;
  {
    std::lock_guard<std::mutex> lock(conn->writeMutex);
    if (conn->closeRequested) return;
    conn->closeRequested = true;
    conn->closeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.closeFlushTimeoutMs);
    conn->writeBuf += BuildCloseFrame(code, reason);
    FlushSome(conn->fd, conn->writeBuf);
  }
  Poke(conn->wake[1]);
}

}  // namespace quest_net
