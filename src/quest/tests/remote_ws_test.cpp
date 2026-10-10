// Host test for the remote WebSocket transport policy and worker (src/quest/net/remote_ws.{h,cpp}) with a
// fake connector standing in for libcurl, wired to the real session router. Covers: wss-only, one attempt
// and no plaintext fallback on a TLS verification failure, login-first ordering through a real worker
// thread, peer close reaching the game sockets, backpressure and shutdown.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "quest/net/remote_ws.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/session_router.h"

using namespace quest_net;
using namespace nevr_session_router;

namespace {

const std::string kSecretUrlPart = "password=hunter2";
const std::string kSecretHeader = "SECRET-JWT-VALUE";
const std::string kUrl = "wss://service.example/nevr?format=evr&" + kSecretUrlPart;

class FakeConnection : public WsConnection {
 public:
  bool Send(std::string_view data, bool) override {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return !gateClosed; });
    if (failSends) return false;
    sent.emplace_back(data);
    return true;
  }
  RecvResult Recv(int timeoutMs) override {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !incoming.empty() || wakePending; });
    wakePending = false;
    if (incoming.empty()) return RecvResult();
    RecvResult r = std::move(incoming.front());
    incoming.pop_front();
    return r;
  }
  void Wake() override {
    std::lock_guard<std::mutex> lock(mutex);
    wakePending = true;
    cv.notify_all();
  }
  void SendClose(uint16_t code) override {
    std::lock_guard<std::mutex> lock(mutex);
    closeCodes.push_back(code);
  }
  void Push(RecvResult r) {
    std::lock_guard<std::mutex> lock(mutex);
    incoming.push_back(std::move(r));
    cv.notify_all();
  }
  std::vector<std::string> Sent() {
    std::lock_guard<std::mutex> lock(mutex);
    return sent;
  }
  void SetGate(bool closed) {
    std::lock_guard<std::mutex> lock(mutex);
    gateClosed = closed;
    cv.notify_all();
  }
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<RecvResult> incoming;
  std::vector<std::string> sent;
  std::vector<uint16_t> closeCodes;
  bool wakePending = false;
  bool gateClosed = false;
  bool failSends = false;
};

class FakeConnector : public WsConnector {
 public:
  ConnectResult Connect(const ConnectRequest& request) override {
    ConnectResult result;
    {
      std::lock_guard<std::mutex> lock(mutex);
      urls.push_back(request.url);
      ++calls;
      result.status = nextStatus;
      result.httpStatus = nextHttp;
      if (nextStatus == ConnectStatus::Ok) {
        auto conn = std::make_unique<FakeConnection>();
        connection = conn.get();
        const std::size_t tag = request.url.find("&conn=");
        if (tag != std::string::npos) byConn[request.url.substr(tag + 6)] = conn.get();
        result.connection = std::move(conn);
      }
    }
    cv.notify_all();
    return result;
  }
  bool WaitForConnection(int ms = 3000) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return connection != nullptr; });
  }
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> urls;
  int calls = 0;
  ConnectStatus nextStatus = ConnectStatus::Ok;
  int nextHttp = 0;
  FakeConnection* connection = nullptr;  // owned by the transport's worker; valid until the session ends
  std::map<std::string, FakeConnection*> byConn;  // by the "&conn=<connIdx>" the builder appends
  FakeConnection* Conn(const std::string& idx) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = byConn.find(idx);
    return it == byConn.end() ? nullptr : it->second;
  }
};

class FakeGames : public GameTransport {
 public:
  SendResult Send(GameId game, std::string_view frame, bool) override {
    std::lock_guard<std::mutex> lock(mutex);
    sent.emplace_back(game, std::string(frame));
    cv.notify_all();
    return SendResult::Sent;
  }
  void Close(GameId game, uint16_t code, std::string_view) override {
    std::lock_guard<std::mutex> lock(mutex);
    closes.emplace_back(game, code);
    cv.notify_all();
  }
  template <class Pred>
  bool WaitFor(Pred pred, int ms = 3000) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), pred);
  }
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::pair<GameId, std::string>> sent;
  std::vector<std::pair<GameId, uint16_t>> closes;
};

const std::string kLogin = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginRequest, "LOGIN");

struct Rig {
  FakeConnector connector;
  FakeGames games;
  std::mutex logMutex;
  std::vector<std::string> logs;
  std::unique_ptr<ConnectorRemoteTransport> transport;
  std::unique_ptr<Router> router;
  RequestBuilder builder;

  explicit Rig(std::size_t maxQueued = 4u << 20) {
    auto sink = [this](LogLevel, const std::string& l) {
      std::lock_guard<std::mutex> lock(logMutex);
      logs.push_back(l);
    };
    builder = [](const RemoteOpenRequest& request) {
      ConnectRequest r;
      r.url = kUrl + "&conn=" + std::to_string(request.connIdx);
      r.headers.push_back({"Authorization", "Bearer " + kSecretHeader});
      return std::optional<ConnectRequest>(r);
    };
    ConnectorRemoteTransport::Config cfg;
    cfg.maxQueuedBytes = maxQueued;
    cfg.pollMs = 20;
    cfg.log = sink;
    transport = std::make_unique<ConnectorRemoteTransport>(&connector, [this](const RemoteOpenRequest& r) { return builder(r); }, cfg);
    Options options;
    options.log = sink;
    options.buildLogin = []() { return std::optional<std::string>(kLogin); };
    router = std::make_unique<Router>(&games, transport.get(), options);
    transport->Attach(router.get());
  }
  ~Rig() {
    transport->Stop();
    router->Shutdown();
  }
  bool LogHas(const std::string& needle) {
    std::lock_guard<std::mutex> lock(logMutex);
    for (const auto& l : logs) {
      if (l.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  // Opens game 1 (config) and 2 (login) and waits for the login remote's connection.
  void OpenLoginGame() {
    router->OnGameOpen(1);
    router->OnGameOpen(2);
  }
};

bool WaitUntil(const std::function<bool()>& pred, int ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

void TestUrlPolicyVectors() {
  QCHECK(IsAcceptableRemoteUrl("wss://host/path"));
  QCHECK(IsAcceptableRemoteUrl("WSS://Host:8443/p?q=1"));
  QCHECK(IsAcceptableRemoteUrl("wss://127.0.0.1:1234"));
  QCHECK(!IsAcceptableRemoteUrl("ws://host/path"));
  QCHECK(!IsAcceptableRemoteUrl("ws://127.0.0.1:1234"));  // loopback is not an exception
  QCHECK(!IsAcceptableRemoteUrl("https://host/"));
  QCHECK(!IsAcceptableRemoteUrl("http://host/"));
  QCHECK(!IsAcceptableRemoteUrl("wss://"));
  QCHECK(!IsAcceptableRemoteUrl("wss:///path"));
  QCHECK(!IsAcceptableRemoteUrl("wss://:80/"));
  QCHECK(!IsAcceptableRemoteUrl("wss://user@host/"));
  QCHECK(!IsAcceptableRemoteUrl("wss://host/a b"));
  QCHECK(!IsAcceptableRemoteUrl("wss://host/\r\nX: y"));
  QCHECK(!IsAcceptableRemoteUrl(""));
  QCHECK(!IsAcceptableRemoteUrl("wsss://host"));
  QCHECK(!IsAcceptableRemoteUrl(" wss://host"));
}

// Failure caught: dialing plaintext. The connector is never reached, the session ends, nothing leaks.
void TestPlaintextUrlIsRefused() {
  Rig rig;
  rig.builder = [](const RemoteOpenRequest&) {
    ConnectRequest r;
    r.url = "ws://service.example/nevr?" + kSecretUrlPart;
    return std::optional<ConnectRequest>(r);
  };
  rig.router->OnGameOpen(1);
  QCHECK(rig.games.WaitFor([&] { return rig.games.closes.size() == 1; }));
  QCHECK(rig.connector.calls == 0);
  QCHECK(rig.LogHas("must be wss://"));
  QCHECK(!rig.LogHas(kSecretUrlPart));
  QCHECK(!rig.LogHas("service.example"));
}

void TestMissingIdentityIsRefused() {
  Rig rig;
  rig.builder = [](const RemoteOpenRequest&) { return std::optional<ConnectRequest>(); };
  rig.router->OnGameOpen(1);
  QCHECK(rig.games.WaitFor([&] { return rig.games.closes.size() == 1; }));
  QCHECK(rig.connector.calls == 0);
}

// Failure caught: a second, weaker attempt after a TLS verification failure (downgrade to ws://, retry with
// verification off, or any retry at all). One attempt, wss, then the game's sockets close.
void TestTlsVerificationFailureHasNoFallback() {
  Rig rig;
  rig.connector.nextStatus = ConnectStatus::TlsVerificationFailed;
  rig.OpenLoginGame();
  QCHECK(rig.games.WaitFor([&] { return rig.games.closes.size() == 2; }));  // config and login each failed once
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // a retry would show up by now
  QCHECK(rig.connector.calls == 2);
  for (const auto& url : rig.connector.urls) QCHECK(url.rfind("wss://", 0) == 0);
  QCHECK(rig.LogHas("tls verification failed"));
  QCHECK(rig.LogHas("no retry, no downgrade"));
  QCHECK(!rig.LogHas(kSecretUrlPart));
  QCHECK(!rig.LogHas(kSecretHeader));
  QCHECK(rig.router->GetStats().remotes == 0);
  // Nothing was ever sent on a session that never verified, in particular no login frame.
  QCHECK(rig.games.sent.empty());
}

// Login first, then the frames the game queued while the connect was in flight, in order, through the real
// worker thread; replies reach the game; a peer close ends the game's sockets.
void TestSessionLifecycle() {
  Rig rig;
  rig.OpenLoginGame();
  rig.router->OnGameFrame(2, nevr_evr_codec::BuildMessage(0x77, "first"), true);
  QCHECK(WaitUntil([&] { return rig.connector.calls == 2; }));
  QCHECK(WaitUntil([&] { return rig.connector.Conn("1") != nullptr; }));
  FakeConnection* login = rig.connector.Conn("1");
  QCHECK(WaitUntil([&] { return !login->Sent().empty(); }));
  const auto sent = login->Sent();
  QCHECK(sent.size() >= 1);
  if (!sent.empty()) QCHECK(sent[0] == kLogin);
  QCHECK(WaitUntil([&] { return login->Sent().size() == 2; }));
  if (login->Sent().size() == 2) QCHECK(login->Sent()[1] == nevr_evr_codec::BuildMessage(0x77, "first"));

  RecvResult reply;
  reply.status = RecvStatus::Frame;
  reply.data = "from-service";
  login->Push(reply);
  QCHECK(rig.games.WaitFor([&] { return !rig.games.sent.empty(); }));
  QCHECK(rig.games.sent[0].first == 2 && rig.games.sent[0].second == "from-service");

  RecvResult closed;
  closed.status = RecvStatus::Closed;
  closed.closeCode = 1006;
  login->Push(closed);
  QCHECK(rig.games.WaitFor([&] { return !rig.games.closes.empty(); }));
  QCHECK(rig.games.closes[0].first == 2);
  QCHECK(rig.router->GetStats().nextConnIdx == 1);
}

// Failure caught: a transport that loses frames or reorders them when the connection is slow, and one
// that never tells the router it can write again.
void TestOutboundBackpressure() {
  Rig rig(/*maxQueued=*/300);
  rig.OpenLoginGame();
  QCHECK(WaitUntil([&] { return rig.connector.calls == 2; }));
  QCHECK(WaitUntil([&] { return rig.connector.Conn("1") != nullptr && rig.connector.Conn("1")->Sent().size() == 1; }));
  FakeConnection* login = rig.connector.Conn("1");
  login->SetGate(true);  // the TLS layer stops taking writes
  std::vector<std::string> frames;
  for (int i = 0; i < 12; ++i) {
    frames.push_back(nevr_evr_codec::BuildMessage(0x88, std::string(50, static_cast<char>('a' + i))));
    rig.router->OnGameFrame(2, frames.back(), true);
  }
  QCHECK(rig.router->GetStats().outboundBytes > 0);  // the router is holding frames the transport refused
  login->SetGate(false);
  QCHECK(WaitUntil([&] { return login->Sent().size() == 13; }));
  login->SetGate(false);
  bool ordered = login->Sent().size() == 13;
  for (std::size_t i = 0; ordered && i < frames.size(); ++i) ordered = login->Sent()[i + 1] == frames[i];
  QCHECK(ordered);
  QCHECK(WaitUntil([&] { return rig.router->GetStats().outboundBytes == 0; }));
}

void TestSendFailureEndsTheSession() {
  Rig rig;
  rig.OpenLoginGame();
  QCHECK(WaitUntil([&] { return rig.connector.calls == 2; }));
  QCHECK(WaitUntil([&] { return rig.connector.Conn("1") != nullptr; }));
  QCHECK(WaitUntil([&] { return rig.connector.Conn("1")->Sent().size() == 1; }));
  rig.connector.Conn("1")->failSends = true;
  rig.router->OnGameFrame(2, nevr_evr_codec::BuildMessage(0x99, "x"), true);
  QCHECK(rig.games.WaitFor([&] { return !rig.games.closes.empty(); }));
}

// Stop sends a going-away close, joins the worker, and the router hears nothing more.
void TestStop() {
  Rig rig;
  rig.OpenLoginGame();
  QCHECK(WaitUntil([&] { return rig.connector.calls == 2; }));
  QCHECK(WaitUntil([&] { return rig.connector.Conn("1") != nullptr; }));
  // The connection object exists before the open has completed. Wait for the login frame to have been sent
  // (as the other tests do), or Stop can land first: the worker's send then fails and the game is closed.
  QCHECK(WaitUntil([&] { return rig.connector.Conn("1")->Sent().size() == 1; }));
  rig.transport->Stop();
  QCHECK(rig.games.closes.empty());  // Stop reports nothing to the router
}

}  // namespace

int main() {
  TestUrlPolicyVectors();
  TestPlaintextUrlIsRefused();
  TestMissingIdentityIsRefused();
  TestTlsVerificationFailureHasNoFallback();
  TestSessionLifecycle();
  TestOutboundBackpressure();
  TestSendFailureEndsTheSession();
  TestStop();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "remote_ws_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("remote_ws_test: all checks passed\n");
  return 0;
}
