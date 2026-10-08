#include "quest/net/loopback_game_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "quest/net/ws_wire.h"

namespace quest_net {

using SessionRouter::GameId;
using SessionRouter::LogLevel;
using SessionRouter::SendResult;

namespace {

std::string Fmt(const char* format, unsigned long long a = 0, unsigned long long b = 0, unsigned long long c = 0) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), format, a, b, c);
  return buf;
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
  std::chrono::steady_clock::time_point closeDeadline;
};

LoopbackGameServer::LoopbackGameServer(Config config) : config_(std::move(config)) {}

LoopbackGameServer::~LoopbackGameServer() { Stop(); }

void LoopbackGameServer::Log(LogLevel level, const std::string& line) {
  if (config_.log) config_.log(level, line);
}

uint16_t LoopbackGameServer::Start() {
  if (router_ == nullptr || listenFd_ >= 0) return 0;
  listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listenFd_ < 0) {
    Log(LogLevel::Error, Fmt("[loopback] socket() failed errno=%llu", static_cast<unsigned long long>(errno)));
    return 0;
  }
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // ephemeral: never a fixed port
  socklen_t len = sizeof(addr);
  if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listenFd_, 8) != 0 ||
      ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0 || !SetNonBlocking(listenFd_)) {
    Log(LogLevel::Error, Fmt("[loopback] bind/listen failed errno=%llu", static_cast<unsigned long long>(errno)));
    CloseFd(listenFd_);
    return 0;
  }
  port_ = ntohs(addr.sin_port);
  MakePipe(wakeFds_);
  stop_ = false;
  acceptThread_ = std::thread([this]() { AcceptLoop(); });
  Log(LogLevel::Info, Fmt("[loopback] listening on 127.0.0.1:%llu", port_));
  return port_;
}

void LoopbackGameServer::Stop() {
  if (listenFd_ < 0 && !acceptThread_.joinable()) return;
  stop_ = true;
  Poke(wakeFds_[1]);
  if (acceptThread_.joinable()) acceptThread_.join();
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
  CloseFd(listenFd_);
  CloseFd(wakeFds_[0]);
  CloseFd(wakeFds_[1]);
  Log(LogLevel::Info, "[loopback] stopped");
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
  while (!stop_) {
    pollfd fds[2] = {{listenFd_, POLLIN, 0}, {wakeFds_[0], POLLIN, 0}};
    const int ready = ::poll(fds, 2, 250);
    Reap();
    if (ready <= 0) continue;
    if ((fds[1].revents & POLLIN) != 0) Drain(wakeFds_[0]);
    if ((fds[0].revents & POLLIN) == 0) continue;
    for (;;) {
      const int fd = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0) break;
      std::size_t count = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        count = conns_.size();
      }
      if (count >= config_.maxConnections || !SetNonBlocking(fd)) {
        Log(LogLevel::Warning, Fmt("[loopback] connection refused: %llu connections already open", count));
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
      conn->thread = std::thread([this, conn]() { ConnLoop(conn); });
    }
  }
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
  const char* endReason = "peer closed";
  std::string inbox;               // handshake bytes
  FrameDecoder decoder(config_.maxMessageBytes);
  bool handshaken = false;

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
      if (closing && !wantWrite) break;  // close frame flushed
      if (closing && std::chrono::steady_clock::now() > conn->closeDeadline) {
        endReason = "close flush timed out";
        break;
      }
    }
    if (!handshaken && std::chrono::steady_clock::now() - started > std::chrono::milliseconds(config_.handshakeTimeoutMs)) {
      endReason = "handshake timed out";
      break;
    }
    const short interest = static_cast<short>((closing ? 0 : POLLIN) | (wantWrite ? POLLOUT : 0));
    pollfd fds[2] = {{conn->fd, interest, 0}, {conn->wake[0], POLLIN, 0}};
    const int ready = ::poll(fds, 2, 200);
    if (ready < 0 && errno != EINTR) {
      endReason = "poll failed";
      break;
    }
    if (ready <= 0) continue;
    if ((fds[1].revents & POLLIN) != 0) Drain(conn->wake[0]);

    if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0) {
      endReason = "socket error";
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
        endReason = "write failed";
        break;
      }
      if (owe && opened) router_->OnGameWritable(id);
    }
    if (closing) continue;
    if ((fds[0].revents & (POLLIN | POLLHUP)) == 0) continue;

    char chunk[16384];
    const ssize_t got = ::recv(conn->fd, chunk, sizeof(chunk), MSG_DONTWAIT);
    if (got == 0) break;
    if (got < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      endReason = "read failed";
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
        endReason = hs == HandshakeStatus::TooLarge ? "handshake too large" : "bad handshake";
        Log(LogLevel::Warning, Fmt("[loopback] conn=%llu handshake rejected", id));
        std::lock_guard<std::mutex> lock(conn->writeMutex);
        FlushSome(conn->fd, conn->writeBuf);
        break;
      }
      queueWrite(BuildUpgradeResponse(request.key), true);
      handshaken = true;
      {
        std::lock_guard<std::mutex> lock(conn->writeMutex);
        if (!FlushSome(conn->fd, conn->writeBuf)) {
          endReason = "write failed";
          break;
        }
      }
      opened = true;
      Log(LogLevel::Info, Fmt("[loopback] conn=%llu upgraded", id));
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
        Log(LogLevel::Error, Fmt("[loopback] conn=%llu message exceeds the %llu byte limit", id, config_.maxMessageBytes));
        beginClose(SessionRouter::kCloseMessageTooBig, "message too big");
        endReason = "message too big";
        break;
      }
      if (status == DecodeStatus::ProtocolError) {
        Log(LogLevel::Warning, Fmt("[loopback] conn=%llu WebSocket protocol error", id));
        beginClose(1002, "protocol error");
        endReason = "protocol error";
        break;
      }
      switch (message.opcode) {
        case Opcode::Text:
        case Opcode::Binary:
          router_->OnGameFrame(id, std::move(message.payload), message.opcode == Opcode::Binary);
          break;
        case Opcode::Ping:
          queueWrite(BuildFrame(Opcode::Pong, message.payload), true);
          break;
        case Opcode::Close:
          beginClose(message.closeCode == 1005 ? static_cast<uint16_t>(1000) : message.closeCode, "");
          endReason = "peer sent close";
          break;
        default:
          break;
      }
    }
    // Opportunistic flush of pongs / close frames queued above.
    {
      std::lock_guard<std::mutex> lock(conn->writeMutex);
      if (!FlushSome(conn->fd, conn->writeBuf)) {
        endReason = "write failed";
        open = false;
      }
    }
  }

  if (decoder.UnmaskedFrames() != 0) {
    Log(LogLevel::Info, Fmt("[loopback] conn=%llu sent %llu frame(s) without the mask bit (accepted)", id, decoder.UnmaskedFrames()));
  }
  Log(LogLevel::Info, std::string("[loopback] conn=") + std::to_string(id) + " ended: " + endReason);
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
