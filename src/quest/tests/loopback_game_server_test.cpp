// Host test for the loopback game server (src/quest/net/loopback_game_server.{h,cpp}) wired to the real
// session router with a fake remote transport. The "game" is a raw TCP client on 127.0.0.1 that speaks the
// client half of RFC 6455 by hand, including one-byte writes (partial reads on the server side).

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

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

// An upgrade request for `target`, with optional extra header lines (each ending in CRLF).
std::string UpgradeRequestText(const std::string& target, const std::string& extra = "") {
  return "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n";
}

struct Rig {
  FakeRemotes remotes;
  std::mutex logMutex;
  std::vector<std::string> logs;
  std::unique_ptr<LoopbackGameServer> server;
  std::unique_ptr<Router> router;
  explicit Rig(std::size_t maxMessage = 1u << 20, std::size_t maxWrite = 8u << 20, int idleFirstFrameMs = 30000,
               const std::function<void(LoopbackGameServer::Config&)>& tweak = nullptr) {
    LoopbackGameServer::Config cfg;
    cfg.maxMessageBytes = maxMessage;
    cfg.maxWriteBufferBytes = maxWrite;
    cfg.handshakeTimeoutMs = 800;
    cfg.idleFirstFrameMs = idleFirstFrameMs;
    if (tweak) tweak(cfg);
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
  // The access token, as the redirect value LoopbackUri() carries it ("ws://127.0.0.1:<port>/<token>/").
  std::string Token() const {
    const std::string uri = server->LoopbackUri();
    const std::size_t slash = uri.find('/', 6);  // after "ws://" and the authority
    return slash == std::string::npos || uri.size() < slash + 1 + 32 ? std::string() : uri.substr(slash + 1, 32);
  }
  std::string GoodTarget(const std::string& rest = "config") const { return "/" + Token() + "/" + rest; }
  bool HasLog(const std::string& needle) {
    std::lock_guard<std::mutex> lock(logMutex);
    for (const auto& l : logs) {
      if (l.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  // The loopback server's records (the router's own lines are not JSON), parsed. A line from the server that
  // does not parse is a failure of the record contract.
  std::vector<nlohmann::json> Records() {
    std::lock_guard<std::mutex> lock(logMutex);
    std::vector<nlohmann::json> out;
    for (const auto& l : logs) {
      if (l.find("\"event\":\"router_") == std::string::npos) continue;
      try {
        out.push_back(nlohmann::json::parse(l));
      } catch (const nlohmann::json::exception&) {
        std::fprintf(stderr, "a loopback record is not valid JSON: %s\n", l.c_str());
        ++quest_test::Failures();
      }
    }
    return out;
  }
  // Records with this event and action (and, when given, this reason or class).
  std::size_t Count(const std::string& event, const std::string& action, const std::string& why = "") {
    std::size_t n = 0;
    for (const nlohmann::json& r : Records()) {
      if (r.value("event", "") != event || r.value("action", "") != action) continue;
      if (!why.empty() && r.value("reason", "") != why && r.value("class", "") != why) continue;
      ++n;
    }
    return n;
  }
  bool WaitForRecord(const std::string& event, const std::string& action, const std::string& why = "",
                     int ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (Count(event, action, why) != 0) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
};

// The descriptor of this process's socket listening on 127.0.0.1:`port`, found the way a foreign component
// would hold it: by number. -1 when there is none.
int ListenerFdFor(uint16_t port) {
  DIR* dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) return -1;
  int found = -1;
  while (dirent* e = ::readdir(dir)) {
    const int fd = std::atoi(e->d_name);
    if (fd <= 2 || fd == ::dirfd(dir)) continue;
    int accepting = 0;
    socklen_t len = sizeof(accepting);
    if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &len) != 0 || accepting == 0) continue;
    sockaddr_in addr{};
    socklen_t alen = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen) != 0 || addr.sin_family != AF_INET) continue;
    if (ntohs(addr.sin_port) == port) found = fd;
  }
  ::closedir(dir);
  return found;
}

bool Upgrade(Rig& rig, Client& c) {
  c.Write(UpgradeRequestText(rig.GoodTarget()), /*oneByteAtATime=*/true);  // partial reads on the server's handshake parser
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
  QCHECK(Upgrade(rig, game));
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
  QCHECK(Upgrade(rig, config));
  QCHECK(rig.remotes.WaitFor([&] { return rig.remotes.opens.size() == 1; }));
  QCHECK(Upgrade(rig, login));
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
  QCHECK(Upgrade(rig, game));
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
  QCHECK(Upgrade(rig, game));
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
  QCHECK(Upgrade(rig, a));
  QCHECK(Upgrade(rig, b));
  QCHECK(rig.remotes.WaitFor([&] { return rig.remotes.opens.size() == 2; }));
  QCHECK(rig.router->GetStats().games == 2);
  rig.server->Stop();
  QCHECK(rig.router->GetStats().games == 0);
  QCHECK(a.WaitEof());
}


// ---- access token ---------------------------------------------------------------------------------

// Failure caught: any local app taking the NEVR-authenticated session (probe: a first-arriving outside
// connection became the login and a later one shared the login remote and read the server's frames).
void TestUpgradeNeedsTheToken() {
  Rig rig;
  const uint16_t port = rig.server->Start();
  const std::string token = rig.Token();
  QCHECK(token.size() == 32);
  for (const char c : token) QCHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  {  // a fresh token per Start(): two servers never share one, and it is not a constant or a repeated byte
    Rig other;
    other.server->Start();
    QCHECK(other.Token().size() == 32 && other.Token() != token);
    QCHECK(token.find_first_not_of(token[0]) != std::string::npos);
  }
  QCHECK(rig.server->LoopbackUri() == "ws://127.0.0.1:" + std::to_string(port) + "/" + token + "/");

  std::string wrong = token;
  wrong.back() = wrong.back() == '0' ? '1' : '0';
  const std::vector<std::string> refused = {"/config", "/", "/" + wrong + "/config", "/x/" + token, "/?nevr_token=" + wrong};
  uint64_t expectRefused = 0;
  for (const std::string& target : refused) {
    Client outsider(port);
    outsider.Write(UpgradeRequestText(target));
    QCHECK(outsider.Read(12).rfind("HTTP/1.1 403", 0) == 0);
    QCHECK(outsider.WaitEof());
    ++expectRefused;
  }
  {  // the right token with an Origin header (a browser or webview)
    Client browser(port);
    browser.Write(UpgradeRequestText(rig.GoodTarget(), "Origin: https://evil.example\r\n"));
    QCHECK(browser.Read(12).rfind("HTTP/1.1 403", 0) == 0);
    QCHECK(browser.WaitEof());
    ++expectRefused;
  }
  QCHECK(rig.server->RejectedUpgrades() == expectRefused);
  // Outsiders consumed no connection number and reached no remote: the game, arriving last, is still conn 0.
  QCHECK(rig.remotes.opens.empty());
  QCHECK(rig.router->GetStats().games == 0);

  // The three accepted spellings of the same token.
  for (const std::string& target : {rig.GoodTarget("rad/config"), "/" + token, "/anything?x=1&nevr_token=" + token}) {
    Client game(port);
    game.Write(UpgradeRequestText(target));
    const std::string resp = game.Read(50, 3000);
    QCHECK(resp.rfind("HTTP/1.1 101", 0) == 0);
  }
  QCHECK(rig.remotes.WaitFor([&] { return rig.remotes.opens.size() == 2; }));  // config + login for the first two
  QCHECK(rig.remotes.opens[0].connIdx == 0);
  QCHECK(rig.server->RejectedUpgrades() == expectRefused);

  // The token is a secret: it is in no log line (the 403 reasons are generic).
  QCHECK(!rig.HasLog(token));
  QCHECK(!rig.HasLog(wrong));
  QCHECK(rig.Count("router_game_conn", "upgrade_refused", "access_token_missing_or_wrong") == 5);
  QCHECK(rig.Count("router_game_conn", "upgrade_refused", "origin_header_present") == 1);
}

// Failure caught: upgraded connections that never speak holding the listener's slots (16) and the game out.
void TestSilentUpgradedConnectionsAreClosed() {
  Rig rig(1u << 20, 8u << 20, /*idleFirstFrameMs=*/400);
  const uint16_t port = rig.server->Start();
  Client silent(port);
  QCHECK(Upgrade(rig, silent));
  Client talker(port);
  QCHECK(Upgrade(rig, talker));
  talker.Write(BuildMaskedFrame(Opcode::Binary, "hello", kMask));
  const std::string expected = BuildCloseFrame(SessionRouter::kClosePolicyViolation, "idle");
  QCHECK(silent.Read(expected.size()) == expected);
  QCHECK(silent.WaitEof());
  QCHECK(rig.server->IdleClosed() == 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  bool eof = false;
  talker.Read(1, 100, &eof);
  QCHECK(!eof);  // a connection that sent a frame is not idle-closed
  QCHECK(rig.server->IdleClosed() == 1);
}

// ---- records and the listener -----------------------------------------------------------------------

// Failure caught (#240): the game's connections reaching the listener and ending with no line saying they
// arrived, were refused, or why they ended. Every accept, rejection and end is one JSON record with a reason.
void TestEveryConnectionIsRecorded() {
  Rig rig(1u << 20, 8u << 20, 30000, [](LoopbackGameServer::Config& c) { c.maxConnections = 2; });
  const uint16_t port = rig.server->Start();
  QCHECK(rig.Count("router_listener", "listening") == 1);
  Client game(port);
  QCHECK(Upgrade(rig, game));
  QCHECK(rig.WaitForRecord("router_game_conn", "upgraded"));
  {
    Client outsider(port);  // second slot: no token
    outsider.Write(UpgradeRequestText("/config"));
    QCHECK(outsider.Read(12).rfind("HTTP/1.1 403", 0) == 0);
    QCHECK(outsider.WaitEof());
  }
  QCHECK(rig.WaitForRecord("router_game_conn", "ended", "upgrade_refused"));
  QCHECK(rig.WaitForRecord("router_game_conn", "upgrade_refused", "access_token_missing_or_wrong"));
  // The refused connection's slot is freed when the accept thread reaps it (one poll period, 250 ms).
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  Client second(port);
  QCHECK(Upgrade(rig, second));
  Client third(port);
  QCHECK(third.WaitEof());  // over the limit: closed without an answer
  QCHECK(rig.WaitForRecord("router_game_conn", "rejected", "connection_limit"));
  QCHECK(rig.Count("router_game_conn", "accepted") == 3);
  bool sawLimit = false;
  for (const nlohmann::json& r : rig.Records()) {
    if (r.value("action", "") == "rejected") sawLimit = r.value("open", 0) == 2 && r.value("limit", 0) == 2;
    if (r.value("action", "") == "ended") QCHECK(r.contains("ms") && r.contains("upgraded"));
  }
  QCHECK(sawLimit);
  QCHECK(!rig.HasLog(rig.Token()));
}

// Failure caught (#240): the listening socket closed by someone else in the process (a stale close of a
// reused number). The game then got ECONNREFUSED every 5 s and the server logged nothing. Now the loss is
// reported with the class that names it, and the same port listens again with the same token.
void TestListenerClosedUnderneathIsReportedAndRestored() {
  Rig rig(1u << 20, 8u << 20, 30000, [](LoopbackGameServer::Config& c) { c.listenerCheckMs = 100; });
  const uint16_t port = rig.server->Start();
  const int fd = ListenerFdFor(port);
  QCHECK(fd >= 0);
  if (fd < 0) return;
  ::close(fd);  // the foreign close
  QCHECK(rig.WaitForRecord("router_listener", "lost", "fd_closed"));
  QCHECK(rig.WaitForRecord("router_listener", "restored"));
  QCHECK(rig.server->ListenerLosses() == 1 && rig.server->ListenerRestores() == 1);
  Client game(port);
  QCHECK(game.fd >= 0);
  QCHECK(Upgrade(rig, game));
  QCHECK(rig.WaitForRecord("router_game_conn", "upgraded"));
}

// The listening number closed and reused by another component's file (dup2 does both at once): reported as
// fd_replaced, the other component's file is left open, and the port listens again.
void TestReplacedListenerNumberIsNotClosed() {
  Rig rig(1u << 20, 8u << 20, 30000, [](LoopbackGameServer::Config& c) { c.listenerCheckMs = 100; });
  const uint16_t port = rig.server->Start();
  const int fd = ListenerFdFor(port);
  QCHECK(fd >= 0);
  if (fd < 0) return;
  int other[2] = {-1, -1};
  QCHECK(::pipe(other) == 0);
  QCHECK(::dup2(other[1], fd) == fd);  // closes the listener, puts the pipe's write end at its number
  QCHECK(rig.WaitForRecord("router_listener", "lost", "fd_replaced"));
  QCHECK(rig.WaitForRecord("router_listener", "restored"));
  const bool wrote = ::write(fd, "x", 1) == 1;  // still the other component's open file
  QCHECK(wrote);
  char byte = 0;
  if (wrote) QCHECK(::read(other[0], &byte, 1) == 1 && byte == 'x');
  Client game(port);
  QCHECK(Upgrade(rig, game));
  ::close(fd);
  ::close(other[0]);
  ::close(other[1]);
}

// Failure caught (#240): the listening socket still held but no longer listening (shut down, or destroyed by
// the kernel the way a socket-destroy request does). Reported as not_listening, closed, listened for again.
void TestListenerThatStopsListeningIsReportedAndRestored() {
  Rig rig(1u << 20, 8u << 20, 30000, [](LoopbackGameServer::Config& c) { c.listenerCheckMs = 100; });
  const uint16_t port = rig.server->Start();
  const int fd = ListenerFdFor(port);
  QCHECK(fd >= 0);
  if (fd < 0) return;
  ::shutdown(fd, SHUT_RDWR);
  QCHECK(rig.WaitForRecord("router_listener", "lost", "not_listening"));
  QCHECK(rig.WaitForRecord("router_listener", "restored"));
  Client game(port);
  QCHECK(game.fd >= 0);
  QCHECK(Upgrade(rig, game));
  QCHECK(rig.server->ListenerLosses() == 1);
}

// A healthy listener is never reported lost, however often it is checked.
void TestHealthyListenerIsNotReported() {
  Rig rig(1u << 20, 8u << 20, 30000, [](LoopbackGameServer::Config& c) { c.listenerCheckMs = 50; });
  const uint16_t port = rig.server->Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  Client game(port);
  QCHECK(Upgrade(rig, game));
  QCHECK(rig.server->ListenerLosses() == 0);
  QCHECK(rig.Count("router_listener", "lost") == 0);
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);  // a write to a socket at a reused number must fail a check, not end the run
  TestRoundTrip();
  TestRemoteCloseReachesTheSocket();
  TestBadHandshake();
  TestOversizedMessage();
  TestSlowReaderBackpressure();
  TestStopClosesEverything();
  TestUpgradeNeedsTheToken();
  TestSilentUpgradedConnectionsAreClosed();
  TestEveryConnectionIsRecorded();
  TestListenerClosedUnderneathIsReportedAndRestored();
  TestListenerThatStopsListeningIsReportedAndRestored();
  TestReplacedListenerNumberIsNotClosed();
  TestHealthyListenerIsNotReported();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "loopback_game_server_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("loopback_game_server_test: all checks passed\n");
  return 0;
}
