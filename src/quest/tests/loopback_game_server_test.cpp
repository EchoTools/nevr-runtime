// Host test for the loopback game server (src/quest/net/loopback_game_server.{h,cpp}) wired to the real
// session router with a fake remote transport. The "game" is a raw TCP client on 127.0.0.1 that speaks the
// client half of RFC 6455 by hand, including one-byte writes (partial reads on the server side).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "quest/net/loopback_game_server.h"
#include "quest/net/ws_wire.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/session_router.h"

using namespace quest_net;
using namespace SessionRouter;

namespace {

const uint8_t kMask[4] = {1, 2, 3, 4};

class FakeRemotes : public RemoteTransport {
 public:
  bool Open(const RemoteOpenRequest& request) override {
    std::lock_guard<std::mutex> lock(mutex);
    opens.push_back(request);
    cv.notify_all();
    return true;
  }
  SendResult Send(RemoteId remote, std::string_view frame, bool) override {
    std::lock_guard<std::mutex> lock(mutex);
    sent.push_back({remote, std::string(frame)});
    cv.notify_all();
    return SendResult::Sent;
  }
  void Close(RemoteId remote, uint16_t) override {
    std::lock_guard<std::mutex> lock(mutex);
    closedRemotes.push_back(remote);
  }
  template <class Pred>
  bool WaitFor(Pred pred, int ms = 3000) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), pred);
  }
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<RemoteOpenRequest> opens;
  std::vector<std::pair<RemoteId, std::string>> sent;
  std::vector<RemoteId> closedRemotes;
};

struct Client {
  int fd = -1;
  explicit Client(uint16_t port) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      fd = -1;
    }
  }
  ~Client() {
    if (fd >= 0) ::close(fd);
  }
  void Write(const std::string& bytes, bool oneByteAtATime = false) {
    if (oneByteAtATime) {
      for (const char c : bytes) {
        ::send(fd, &c, 1, MSG_NOSIGNAL);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    } else {
      std::size_t off = 0;
      while (off < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += static_cast<std::size_t>(n);
      }
    }
  }
  // Reads until `want` bytes are available or `ms` pass; returns what arrived. Stops early at EOF.
  std::string Read(std::size_t want, int ms = 3000, bool* eof = nullptr) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (out.size() < want && std::chrono::steady_clock::now() < deadline) {
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, 50) <= 0) continue;
      char buf[65536];
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n == 0) {
        if (eof) *eof = true;
        break;
      }
      if (n < 0) break;
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }
  bool WaitEof(int ms = 3000) {
    bool eof = false;
    Read(1u << 30, ms, &eof);
    return eof;
  }
};

const std::string kUpgrade =
    "GET /config HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

struct Rig {
  FakeRemotes remotes;
  std::mutex logMutex;
  std::vector<std::string> logs;
  std::unique_ptr<LoopbackGameServer> server;
  std::unique_ptr<Router> router;
  explicit Rig(std::size_t maxMessage = 1u << 20, std::size_t maxWrite = 8u << 20) {
    LoopbackGameServer::Config cfg;
    cfg.maxMessageBytes = maxMessage;
    cfg.maxWriteBufferBytes = maxWrite;
    cfg.handshakeTimeoutMs = 800;
    cfg.log = [this](LogLevel, const std::string& l) {
      std::lock_guard<std::mutex> lock(logMutex);
      logs.push_back(l);
    };
    server = std::make_unique<LoopbackGameServer>(cfg);
    Options options;
    options.limits.maxFrameBytes = maxMessage;
    options.limits.maxOutboundBytes = 64u << 20;  // the slow-reader test queues ~24 MB behind a full socket
    options.buildLogin = []() { return std::optional<std::string>(EvrCodec::BuildMessage(EvrCodec::kSymLoginRequest, "L")); };
    options.log = cfg.log;
    router = std::make_unique<Router>(server.get(), &remotes, options);
    server->Attach(router.get());
  }
  ~Rig() {
    server->Stop();
    router->Shutdown();
  }
  bool HasLog(const std::string& needle) {
    std::lock_guard<std::mutex> lock(logMutex);
    for (const auto& l : logs) {
      if (l.find(needle) != std::string::npos) return true;
    }
    return false;
  }
};

bool Upgrade(Client& c) {
  c.Write(kUpgrade, /*oneByteAtATime=*/true);  // partial reads on the server's handshake parser
  const std::string resp = c.Read(50, 3000);
  return resp.rfind("HTTP/1.1 101", 0) == 0 && resp.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos;
}

// Handshake, frames in pieces, reply path, and the first connection becoming "config".
void TestRoundTrip() {
  Rig rig;
  const uint16_t port = rig.server->Start();
  QCHECK(port != 0);
  Client game(port);
  QCHECK(game.fd >= 0);
  QCHECK(Upgrade(game));
  QCHECK(rig.remotes.WaitFor([&] { return rig.remotes.opens.size() == 1; }));
  const RemoteId remote = rig.remotes.opens.empty() ? 0 : rig.remotes.opens[0].remote;
  QCHECK(rig.remotes.opens[0].role == Role::Config);

  rig.router->OnRemoteOpen(remote);
  const std::string payload = EvrCodec::BuildMessage(0x1234, std::string(40000, 'p'));  // > one TCP chunk
  game.Write(BuildMaskedFrame(Opcode::Binary, payload, kMask), /*oneByteAtATime=*/false);
  QCHECK(rig.remotes.WaitFor([&] { return rig.remotes.sent.size() == 1; }));
  QCHECK(rig.remotes.sent[0].second == payload);

  rig.router->OnRemoteFrame(remote, "reply-from-service", true);
  const std::string wire = BuildFrame(Opcode::Binary, "reply-from-service");
  QCHECK(game.Read(wire.size()) == wire);

  // A ping is answered with a pong carrying the same payload.
  game.Write(BuildMaskedFrame(Opcode::Ping, "pp", kMask));
  const std::string pong = BuildFrame(Opcode::Pong, "pp");
  QCHECK(game.Read(pong.size()) == pong);
}

// Failure caught: the game's sockets staying open after the remote session ended (#70).
void TestRemoteCloseReachesTheSocket() {
  Rig rig;
  const uint16_t port = rig.server->Start();
  Client config(port), login(port);
  QCHECK(Upgrade(config));
  QCHECK(rig.remotes.WaitFor([&] { return rig.remotes.opens.size() == 1; }));
  QCHECK(Upgrade(login));
  QCHECK(rig.remotes.WaitFor([&] { return rig.remotes.opens.size() == 2; }));
  const RemoteId loginRemote = rig.remotes.opens[1].remote;
  rig.router->OnRemoteOpen(loginRemote);
  rig.router->OnRemoteClose(loginRemote, 1006);
  const std::string expected = BuildCloseFrame(kCloseGoingAway, "remote closed, code=1006");
  QCHECK(login.Read(expected.size()) == expected);
  QCHECK(login.WaitEof());
  QCHECK(rig.router->GetStats().nextConnIdx == 1);
}

// Failure caught: a connection that is not a WebSocket upgrade reaching the router, or hanging forever.
void TestBadHandshake() {
  Rig rig;
  const uint16_t port = rig.server->Start();
  Client bad(port);
  bad.Write("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
  QCHECK(bad.Read(12).rfind("HTTP/1.1 400", 0) == 0);
  QCHECK(bad.WaitEof());
  Client silent(port);  // never sends a byte: dropped at the handshake timeout
  QCHECK(silent.WaitEof(4000));
  QCHECK(rig.remotes.opens.empty());
  QCHECK(rig.router->GetStats().games == 0);
}

// Failure caught: an oversized WebSocket message being buffered or forwarded.
void TestOversizedMessage() {
  Rig rig(/*maxMessage=*/1000);
  const uint16_t port = rig.server->Start();
  Client game(port);
  QCHECK(Upgrade(game));
  QCHECK(rig.remotes.WaitFor([&] { return rig.remotes.opens.size() == 1; }));
  rig.router->OnRemoteOpen(rig.remotes.opens[0].remote);
  game.Write(BuildMaskedFrame(Opcode::Binary, std::string(2000, 'x'), kMask));
  const std::string expected = BuildCloseFrame(kCloseMessageTooBig, "message too big");
  QCHECK(game.Read(expected.size()) == expected);
  QCHECK(game.WaitEof());
  QCHECK(rig.remotes.sent.empty());
  QCHECK(rig.router->GetStats().games == 0);
}

// Backpressure: the game stops reading; the server keeps a bounded buffer and the router queues the rest;
// when the game reads again every byte arrives, in order, exactly once.
void TestSlowReaderBackpressure() {
  Rig rig(/*maxMessage=*/1u << 20, /*maxWrite=*/256u << 10);
  const uint16_t port = rig.server->Start();
  Client game(port);
  QCHECK(Upgrade(game));
  QCHECK(rig.remotes.WaitFor([&] { return rig.remotes.opens.size() == 1; }));
  const RemoteId remote = rig.remotes.opens[0].remote;
  rig.router->OnRemoteOpen(remote);
  constexpr int kFrames = 400;
  constexpr std::size_t kSize = 60000;  // 24 MB total: far beyond socket buffers and the 256 KiB cap
  std::thread producer([&]() {
    for (int i = 0; i < kFrames; ++i) {
      std::string f(kSize, static_cast<char>('a' + (i % 26)));
      rig.router->OnRemoteFrame(remote, std::move(f), true);
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // game is not reading yet
  QCHECK(rig.router->GetStats().outboundBytes > 0);
  // Now read everything and check order.
  FrameDecoder d(1u << 20);
  int got = 0;
  bool ordered = true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (got < kFrames && std::chrono::steady_clock::now() < deadline) {
    const std::string bytes = game.Read(1, 200);
    if (!bytes.empty()) d.Feed(bytes.data(), bytes.size());
    Message m;
    while (d.Next(&m) == DecodeStatus::Message) {
      if (m.payload.size() != kSize || m.payload[0] != static_cast<char>('a' + (got % 26))) ordered = false;
      ++got;
    }
  }
  producer.join();
  QCHECK(got == kFrames);
  QCHECK(ordered);
}

// Stop() reports every upgraded connection to the router before returning.
void TestStopClosesEverything() {
  Rig rig;
  const uint16_t port = rig.server->Start();
  Client a(port), b(port);
  QCHECK(Upgrade(a));
  QCHECK(Upgrade(b));
  QCHECK(rig.remotes.WaitFor([&] { return rig.remotes.opens.size() == 2; }));
  QCHECK(rig.router->GetStats().games == 2);
  rig.server->Stop();
  QCHECK(rig.router->GetStats().games == 0);
  QCHECK(a.WaitEof());
}

}  // namespace

int main() {
  TestRoundTrip();
  TestRemoteCloseReachesTheSocket();
  TestBadHandshake();
  TestOversizedMessage();
  TestSlowReaderBackpressure();
  TestStopClosesEverything();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "loopback_game_server_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("loopback_game_server_test: all checks passed\n");
  return 0;
}
