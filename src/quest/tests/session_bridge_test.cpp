// Host test for the session bridge (src/quest/net/session_bridge.*): the real loopback
// server, the real session router and the real remote transport, with a fake WebSocket connector in
// place of libcurl and a raw TCP "game". Covers: the auth route, that the router injects NO login (the
// game's own rewritten login is the first frame on the session), the tap in both directions, the
// LoginSuccess signal, the side channel's refusal before the login is accepted, and the fail-closed
// start conditions.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "quest/net/session_bridge.h"
#include "quest/net/ws_wire.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/evr_codec.h"

using namespace quest_net;

namespace {

const uint8_t kMask[4] = {9, 8, 7, 6};

class FakeConnection : public WsConnection {
 public:
  bool Send(std::string_view data, bool) override {
    std::lock_guard<std::mutex> lock(mutex);
    sent.emplace_back(data);
    cv.notify_all();
    return true;
  }
  RecvResult Recv(int timeoutMs) override {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !incoming.empty() || wake; });
    wake = false;
    if (incoming.empty()) return RecvResult();
    RecvResult r = std::move(incoming.front());
    incoming.pop_front();
    return r;
  }
  void Wake() override {
    std::lock_guard<std::mutex> lock(mutex);
    wake = true;
    cv.notify_all();
  }
  void SendClose(uint16_t) override {}
  // The remote breaks under the session (a network error).
  void Fail() {
    std::lock_guard<std::mutex> lock(mutex);
    RecvResult r;
    r.status = RecvStatus::Error;
    incoming.push_back(std::move(r));
    cv.notify_all();
  }
  void Push(std::string data) {
    std::lock_guard<std::mutex> lock(mutex);
    RecvResult r;
    r.status = RecvStatus::Frame;
    r.data = std::move(data);
    r.binary = true;
    incoming.push_back(std::move(r));
    cv.notify_all();
  }
  std::vector<std::string> Sent() {
    std::lock_guard<std::mutex> lock(mutex);
    return sent;
  }
  bool WaitSent(std::size_t n, int ms = 3000) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return sent.size() >= n; });
  }
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<RecvResult> incoming;
  std::vector<std::string> sent;
  bool wake = false;
};

class FakeConnector : public WsConnector {
 public:
  ConnectResult Connect(const ConnectRequest& request) override {
    ConnectResult result;
    result.status = ConnectStatus::Ok;
    auto conn = std::make_unique<FakeConnection>();
    {
      std::lock_guard<std::mutex> lock(mutex);
      requests.push_back(request);
      connections.push_back(conn.get());
    }
    cv.notify_all();
    result.connection = std::move(conn);
    return result;
  }
  bool WaitConnects(std::size_t n, int ms = 3000) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return connections.size() >= n; });
  }
  std::size_t Calls() {
    std::lock_guard<std::mutex> lock(mutex);
    return connections.size();
  }
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<ConnectRequest> requests;
  std::vector<FakeConnection*> connections;  // owned by the transport's workers
};

struct Client {
  int fd = -1;
  explicit Client(uint16_t port) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, quest_net::kListenAddress, &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      fd = -1;
    }
  }
  ~Client() {
    if (fd >= 0) ::close(fd);
  }
  void Write(const std::string& bytes) {
    std::size_t off = 0;
    while (off < bytes.size()) {
      const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
      if (n <= 0) break;
      off += static_cast<std::size_t>(n);
    }
  }
  std::string Read(std::size_t want, int ms = 3000, bool* eof = nullptr) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (out.size() < want && std::chrono::steady_clock::now() < deadline) {
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, 50) <= 0) continue;
      char buf[65536];
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n == 0) {
        if (eof != nullptr) *eof = true;
        break;
      }
      if (n < 0) break;
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }
  // Sends the upgrade request and returns every byte that arrived with the first read: the 101 response, and
  // anything the server sent right behind it (a close, for a connection it refuses at once).
  std::string UpgradeBytes(const std::string& path = "/x") {
    Write("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.2\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    return Read(50);
  }
  bool Upgrade(const std::string& path = "/x") { return UpgradeBytes(path).rfind("HTTP/1.1 101", 0) == 0; }
};

// "/<token>/" out of "ws://127.0.0.2:<port>/<token>/".
std::string PathOf(const std::string& uri) {
  const std::size_t scheme = uri.find("://");
  const std::size_t slash = uri.find('/', scheme == std::string::npos ? 0 : scheme + 3);
  return slash == std::string::npos ? std::string() : uri.substr(slash);
}

struct Observed {
  std::mutex mutex;
  std::vector<std::pair<bool, std::string>> frames;
  std::vector<std::uint64_t> logins;
};

SessionBridge::Config MakeConfig(FakeConnector* connector, Observed* seen, const std::string& jwt,
                                    const std::string& uri = "wss://service.example/nevr") {
  SessionBridge::Config c;
  c.remoteUri = uri;
  c.connector = connector;
  c.identity = [jwt] {
    Identity id;
    id.jwt = jwt;
    id.serverKey = "SERVER-KEY";
    return id;
  };
  c.loopback.handshakeTimeoutMs = 800;
  c.tap.observe = [seen](bool s2g, const std::uint8_t* d, std::size_t n) {
    std::lock_guard<std::mutex> lock(seen->mutex);
    seen->frames.emplace_back(s2g, std::string(reinterpret_cast<const char*>(d), n));
  };
  c.tap.onLoginSuccess = [seen](std::uint64_t a) {
    std::lock_guard<std::mutex> lock(seen->mutex);
    seen->logins.push_back(a);
  };
  return c;
}

bool HasFrame(Observed& seen, bool s2g, const std::string& frame) {
  std::lock_guard<std::mutex> lock(seen.mutex);
  for (const auto& f : seen.frames) if (f.first == s2g && f.second == frame) return true;
  return false;
}

bool WaitFor(const std::function<bool()>& pred, int ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

void TestLoginRelayTapAndSideChannel() {
  FakeConnector connector;
  Observed seen;
  SessionBridge::Config withFriends = MakeConfig(&connector, &seen, "JWT-A");
  withFriends.subscribeFriendList = true;
  SessionBridge bridge(std::move(withFriends));
  const uint16_t port = bridge.Start();
  QCHECK(port != 0);

  Client config(port), login(port);
  QCHECK(config.fd >= 0 && login.fd >= 0);
  const std::string path = PathOf(bridge.LocalUri());
  QCHECK(!path.empty() && path.size() > 3);
  QCHECK(config.Upgrade(path));
  QCHECK(connector.WaitConnects(1));
  QCHECK(login.Upgrade(path));
  QCHECK(connector.WaitConnects(2));

  // The token-auth route: a Bearer JWT on every upgrade, the configured URI unchanged.
  {
    std::lock_guard<std::mutex> lock(connector.mutex);
    for (const ConnectRequest& r : connector.requests) {
      QCHECK(r.url == "wss://service.example/nevr");
      QCHECK(r.headers.size() == 1 && r.headers[0].name == "Authorization" && r.headers[0].value == "Bearer JWT-A");
    }
  }

  // Nothing may go out on the login session before the service accepts the login.
  QCHECK(!bridge.SendToLogin(nevr_evr_codec::BuildMessage(0x77, "early")));

  // The game's own login (the rewritten one) is the first and only frame the service gets: the router
  // injected no second LoginRequest.
  const std::string gameLogin = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginRequest, "game-own-login");
  login.Write(BuildMaskedFrame(Opcode::Binary, gameLogin, kMask));
  FakeConnection* loginConn = nullptr;
  QCHECK(WaitFor([&] {
    std::lock_guard<std::mutex> lock(connector.mutex);
    for (FakeConnection* c : connector.connections) {
      if (!c->Sent().empty()) { loginConn = c; return true; }
    }
    return false;
  }));
  QCHECK(loginConn != nullptr);
  if (loginConn == nullptr) return;
  QCHECK(loginConn->Sent().size() == 1);
  QCHECK(loginConn->Sent()[0] == gameLogin);
  QCHECK(WaitFor([&] { return HasFrame(seen, /*s2g=*/false, gameLogin); }));  // the game->server tap

  // The service accepts: the game receives LoginSuccess, the tap signals the account, the side channel opens.
  const std::string success = nevr_evr_codec::BuildLoginSuccess(nevr_evr_codec::kBridgeLoginPlatform, 31337);
  loginConn->Push(success);
  const std::string wire = BuildFrame(Opcode::Binary, success);
  QCHECK(login.Read(wire.size()) == wire);
  QCHECK(WaitFor([&] {
    std::lock_guard<std::mutex> lock(seen.mutex);
    return seen.logins.size() == 1 && seen.logins[0] == 31337;
  }));
  QCHECK(HasFrame(seen, /*s2g=*/true, success));

  // With social on, the friend-list subscribe follows the accepted login (the game never sends it).
  QCHECK(WaitFor([&] {
    for (const std::string& s : loginConn->Sent()) if (s == nevr_evr_codec::BuildFriendListSubscribe()) return true;
    return false;
  }));

  const std::string request = nevr_evr_codec::BuildMessage(0x1234, "party-request");
  QCHECK(bridge.SendToLogin(request));
  QCHECK(WaitFor([&] {
    for (const std::string& s : loginConn->Sent()) if (s == request) return true;
    return false;
  }));
  // The side channel's own frames are not shown to the tap as game frames.
  QCHECK(!HasFrame(seen, /*s2g=*/false, request));
  bridge.Stop();
}

// The listener is reachable by any local app; without the per-start token an upgrade is refused and
// nothing reaches the service.
void TestUpgradeWithoutTheTokenIsRefused() {
  FakeConnector connector;
  Observed seen;
  SessionBridge bridge(MakeConfig(&connector, &seen, "JWT-A"));
  const uint16_t port = bridge.Start();
  QCHECK(port != 0);
  QCHECK(bridge.LocalUri().rfind("ws://127.0.0.2:", 0) == 0);
  Client intruder(port);
  QCHECK(intruder.fd >= 0);
  QCHECK(!intruder.Upgrade("/"));
  QCHECK(!intruder.Upgrade("/not-the-token/"));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  QCHECK(connector.Calls() == 0);
  bridge.Stop();
}

void TestNoJwtMeansNoSessionAndTheGameSocketCloses() {
  FakeConnector connector;
  Observed seen;
  SessionBridge bridge(MakeConfig(&connector, &seen, ""));
  const uint16_t port = bridge.Start();
  QCHECK(port != 0);
  Client game(port);
  QCHECK(game.Upgrade(PathOf(bridge.LocalUri())));
  bool eof = false;
  game.Read(1u << 20, 3000, &eof);
  QCHECK(connector.Calls() == 0);  // no unauthenticated session, ever
  bridge.Stop();
}

// The gate and the account the identity callback answers with, driven by the test like the token-auth poll.
struct Account {
  std::mutex mutex;
  std::string jwt;
  std::atomic<int> gate{static_cast<int>(nevr_session_router::LoginGate::Awaiting)};
  std::string Jwt() {
    std::lock_guard<std::mutex> lock(mutex);
    return jwt;
  }
  void Set(const std::string& value, nevr_session_router::LoginGate g) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      jwt = value;
    }
    gate = static_cast<int>(g);
  }
};

SessionBridge::Config MakeHeldConfig(FakeConnector* connector, Observed* seen, Account* account) {
  SessionBridge::Config c = MakeConfig(connector, seen, "");
  c.identity = [account] {
    Identity id;
    id.jwt = account->Jwt();
    id.serverKey = "";
    return id;
  };
  c.loginGate = [account] { return static_cast<nevr_session_router::LoginGate>(account->gate.load()); };
  c.loopback.idleFirstFrameMs = 300;
  return c;
}

// The #239 smoke sequence end to end: no account at boot, so the config connection fails and the login
// connection is held (open past the idle window, answered to pings, no remote). The player signs in: the
// remote opens with the new token, the game's login goes through, and the NEW config connection the game
// opens afterwards gets its own remote while the login session's profile reply reaches the login connection.
void TestHeldLoginSurvivesUntilSignInThenRoutesByRole() {
  FakeConnector connector;
  Observed seen;
  Account account;
  SessionBridge bridge(MakeHeldConfig(&connector, &seen, &account));
  const uint16_t port = bridge.Start();
  QCHECK(port != 0);
  const std::string path = PathOf(bridge.LocalUri());

  Client bootConfig(port);
  std::string bootBytes = bootConfig.UpgradeBytes(path);
  QCHECK(bootBytes.rfind("HTTP/1.1 101", 0) == 0);
  bootConfig.Write(BuildMaskedFrame(Opcode::Binary, nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConfigRequest, "c"), kMask));
  bool bootEof = false;
  const std::string closeFrame = BuildCloseFrame(nevr_session_router::kCloseInternalError, "remote could not be started");
  // Fail-fast, as before: the connection is closed 1011 (the close frame can share a read with the upgrade
  // response, so everything up to the end of the stream is read).
  bootBytes += bootConfig.Read(1u << 20, 3000, &bootEof);
  QCHECK(bootBytes.find(closeFrame) != std::string::npos);
  QCHECK(connector.Calls() == 0);

  Client login(port);
  QCHECK(login.Upgrade(path));
  QCHECK(WaitFor([&] { return bridge.HeldLogins() == 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(900));  // three idle windows
  login.Write(BuildMaskedFrame(Opcode::Ping, "pp", kMask));
  const std::string pong = BuildFrame(Opcode::Pong, "pp");
  QCHECK(login.Read(pong.size()) == pong);
  QCHECK(connector.Calls() == 0);

  account.Set("JWT-SIGNED-IN", nevr_session_router::LoginGate::Ready);
  bridge.ReevaluateLoginGate();
  QCHECK(connector.WaitConnects(1));
  {
    std::lock_guard<std::mutex> lock(connector.mutex);
    QCHECK(connector.requests.size() == 1);
    if (!connector.requests.empty()) {
      QCHECK(connector.requests[0].headers.size() == 1 && connector.requests[0].headers[0].value == "Bearer JWT-SIGNED-IN");
    }
  }
  const std::string gameLogin = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginRequest, "game-own-login");
  login.Write(BuildMaskedFrame(Opcode::Binary, gameLogin, kMask));
  FakeConnection* loginConn = nullptr;
  QCHECK(WaitFor([&] {
    std::lock_guard<std::mutex> lock(connector.mutex);
    if (connector.connections.empty()) return false;
    loginConn = connector.connections[0];
    return !loginConn->Sent().empty();
  }));
  if (loginConn == nullptr) return;
  QCHECK(loginConn->Sent()[0] == gameLogin);
  const std::string success = nevr_evr_codec::BuildLoginSuccess(nevr_evr_codec::kBridgeLoginPlatform, 4242);
  loginConn->Push(success);
  const std::string successWire = BuildFrame(Opcode::Binary, success);
  QCHECK(login.Read(successWire.size()) == successWire);

  Client config(port);  // the connection the game opens after login
  QCHECK(config.Upgrade(path));
  config.Write(BuildMaskedFrame(Opcode::Binary, nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConfigRequest, "c2"), kMask));
  QCHECK(connector.WaitConnects(2));
  FakeConnection* configConn = nullptr;
  QCHECK(WaitFor([&] {
    std::lock_guard<std::mutex> lock(connector.mutex);
    if (connector.connections.size() < 2) return false;
    configConn = connector.connections[1];
    return !configConn->Sent().empty();
  }));
  if (configConn == nullptr) return;
  QCHECK(configConn->Sent()[0] == nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConfigRequest, "c2"));

  const std::string profile = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile");
  loginConn->Push(profile);
  const std::string profileWire = BuildFrame(Opcode::Binary, profile);
  QCHECK(login.Read(profileWire.size()) == profileWire);  // the login connection's, not the newest socket's
  // An Unrequire nothing is owed for is dropped, not delivered, and counted where production reads it.
  QCHECK(bridge.DroppedUnrequires() == 0);
  loginConn->Push(nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConnectionUnrequire, ""));
  QCHECK(WaitFor([&] { return bridge.DroppedUnrequires() == 1; }));
  const std::string configReply = nevr_evr_codec::BuildMessage(0xb9cdaf586f7bd012ULL, "config");
  configConn->Push(configReply);
  const std::string configWire = BuildFrame(Opcode::Binary, configReply);
  QCHECK(config.Read(configWire.size()) == configWire);
  bool configGotProfile = false;
  config.Read(1, 200, &configGotProfile);
  QCHECK(!configGotProfile);
  // An Unrequire inside a frame can reach the game with nothing outstanding; the bridge reports the count.
  const std::string reply = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')) +
                            nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConnectionUnrequire, "");
  loginConn->Push(reply);  // the login request is still outstanding: covered
  loginConn->Push(reply);  // nothing left to cover
  QCHECK(WaitFor([&] { return bridge.UnmatchedEmbeddedUnrequires() == 1; }));
  bridge.Stop();
}

// A sign-in that fails ends the hold: the login connection is closed (1011).
void TestHeldLoginIsClosedWhenSignInFails() {
  FakeConnector connector;
  Observed seen;
  Account account;
  SessionBridge bridge(MakeHeldConfig(&connector, &seen, &account));
  const uint16_t port = bridge.Start();
  QCHECK(port != 0);
  const std::string path = PathOf(bridge.LocalUri());
  Client config(port), login(port);
  QCHECK(config.Upgrade(path));
  QCHECK(login.Upgrade(path));
  QCHECK(WaitFor([&] { return bridge.HeldLogins() == 1; }));
  account.Set("", nevr_session_router::LoginGate::Refused);
  bridge.ReevaluateLoginGate();
  const std::string closeFrame = BuildCloseFrame(nevr_session_router::kCloseInternalError, "the account the login needs will not be available");
  QCHECK(login.Read(closeFrame.size()) == closeFrame);
  QCHECK(connector.Calls() == 0);
  bridge.Stop();
}

void TestPlaintextRemoteUriDoesNotStart() {
  FakeConnector connector;
  Observed seen;
  SessionBridge bridge(MakeConfig(&connector, &seen, "JWT", "ws://service.example/nevr"));
  QCHECK(bridge.Start() == 0);
  QCHECK(connector.Calls() == 0);
}

void TestSideChannelRefusesWhenNothingIsConnected() {
  FakeConnector connector;
  Observed seen;
  SessionBridge bridge(MakeConfig(&connector, &seen, "JWT"));
  QCHECK(bridge.Start() != 0);
  QCHECK(!bridge.SendToLogin(nevr_evr_codec::BuildMessage(1, "x")));
  bridge.Stop();
}

// #320: the remote session fails under a logged-in game; the game reconnects its login socket WITHOUT a new
// login. The bridge sends that socket a login-removed notice for the account the service logged in, so the
// game shows RETRY and logs in itself; nothing of the old login is replayed to the new session. Everything real
// except the service: the loopback server, the router, the remote transport.
void TestReconnectedLoginSocketIsToldItsLoginWasRemoved() {
  FakeConnector connector;
  Observed seen;
  SessionBridge::Config cfg = MakeConfig(&connector, &seen, "JWT-A");
  cfg.loopback.silentNotifyMs = 300;  // the silent login socket is recognised after 0.3 s here (1.5 s in production)
  SessionBridge bridge(std::move(cfg));
  const uint16_t port = bridge.Start();
  QCHECK(port != 0);
  const std::string path = PathOf(bridge.LocalUri());

  Client config(port), login(port);
  QCHECK(config.Upgrade(path));
  QCHECK(connector.WaitConnects(1));
  QCHECK(login.Upgrade(path));
  QCHECK(connector.WaitConnects(2));
  const std::string gameLogin = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginRequest, "game-own-login-SECRET");
  login.Write(BuildMaskedFrame(Opcode::Binary, gameLogin, kMask));
  FakeConnection* first = nullptr;
  QCHECK(WaitFor([&] {
    std::lock_guard<std::mutex> lock(connector.mutex);
    for (FakeConnection* c : connector.connections) {
      if (!c->Sent().empty()) { first = c; return true; }
    }
    return false;
  }));
  if (first == nullptr) return;
  // The service's reply carries its Unrequire in the same frame: nothing is outstanding afterwards.
  const std::string success = nevr_evr_codec::BuildLoginSuccess(nevr_evr_codec::kBridgeLoginPlatform, 5150) +
                              nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConnectionUnrequire, "u");
  first->Push(success);
  const std::string successWire = BuildFrame(Opcode::Binary, success);
  QCHECK(login.Read(successWire.size()) == successWire);

  // The network drops the session; the router closes the game's sockets; the game reconnects its login socket.
  const std::size_t connectsBefore = connector.Calls();
  first->Fail();
  bool eof = false;
  login.Read(1u << 16, 3000, &eof);  // the router's close frame, then the end of the socket
  QCHECK(eof);
  // Armed by the loss, before the login socket reconnects: only a bridge that handed the router the notice JSON
  // arms it (the silence report that sends it comes 0.3 s after the reconnect, so this is not yet consumed).
  QCHECK(WaitFor([&] { return bridge.LoginRemovedDue(); }));
  Client reconnected(port);
  QCHECK(reconnected.Upgrade(path));
  QCHECK(connector.WaitConnects(connectsBefore + 1));

  // The silent login socket is told its login was removed (the frame, as the game parses it).
  const std::string notice = nevr_evr_codec::BuildLoginRemovedNotify(
      {nevr_evr_codec::kBridgeLoginPlatform, 5150}, nevr_evr_codec::kLoginRemovedReasonText,
      "{\"message\":\"Connection lost. Select RETRY to sign in again.\"}");
  const std::string noticeWire = BuildFrame(Opcode::Binary, notice);
  QCHECK(reconnected.Read(noticeWire.size()) == noticeWire);
  // Sent because the loopback server reported the socket silent (the router counts it, one notice).
  QCHECK(WaitFor([&] { return bridge.LoginsRemoved() == 1; }));
  QCHECK(bridge.LoginRemovedDue());  // armed until the game's own login: this socket may not be the login peer

  // No credential was replayed: the new session has been sent nothing of the game's old login.
  FakeConnection* second = nullptr;
  {
    std::lock_guard<std::mutex> lock(connector.mutex);
    if (connector.connections.size() > connectsBefore) second = connector.connections.back();
  }
  QCHECK(second != nullptr);
  if (second != nullptr) QCHECK(second->Sent().empty());

  // The game's own login (RETRY) then goes through the same socket as the first frame on the new session.
  const std::string retry = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginRequest, "game-retry-login");
  reconnected.Write(BuildMaskedFrame(Opcode::Binary, retry, kMask));
  if (second != nullptr) {
    QCHECK(second->WaitSent(1));
    QCHECK(second->Sent()[0] == retry);
  }
  QCHECK(WaitFor([&] { return !bridge.LoginRemovedDue(); }));  // the game's own login disarms it
  bridge.Stop();
}

}  // namespace

int main() {
  TestLoginRelayTapAndSideChannel();
  TestUpgradeWithoutTheTokenIsRefused();
  TestNoJwtMeansNoSessionAndTheGameSocketCloses();
  TestHeldLoginSurvivesUntilSignInThenRoutesByRole();
  TestHeldLoginIsClosedWhenSignInFails();
  TestPlaintextRemoteUriDoesNotStart();
  TestSideChannelRefusesWhenNothingIsConnected();
  TestReconnectedLoginSocketIsToldItsLoginWasRemoved();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "session_bridge_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("session_bridge_test: all checks passed\n");
  return 0;
}
