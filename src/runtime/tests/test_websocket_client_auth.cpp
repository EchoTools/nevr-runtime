// Issue #39: the ServerDB bearer token on reconnect.
//
// WebSocketClient::Connect hands the bearer token to ixwebsocket once, as an
// extra header. ixwebsocket's automatic reconnect calls WebSocket::connect()
// again, and that copies the STORED _extraHeaders (IXWebSocket.cpp:210 in
// v11.4.6) — so every reconnect presents the token that was valid at the first
// Connect. Nakama rejects an expired JWT at the upgrade with 401
// (socket_ws.go / nevr_ws_acceptor.go -> parseToken, jwt.WithExpirationRequired),
// so a reconnect after the token's TTL can never succeed on its own.
//
// These tests drive the real ixwebsocket client against a scripted upgrade
// server on loopback and record the Authorization header of every handshake.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <gtest/gtest.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketHandshakeKeyGen.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/server/bearer_reconnect_auth.h"
#include "runtime/server/serialized_mint.h"
#include "runtime/server/websocket_client.h"
#include "runtime/tests/test_log_cap.h"

namespace {
std::mutex g_logMutex;
std::vector<std::string> g_logLines;
}  // namespace

VOID Log(EchoVR::LogLevel, const CHAR* format, ...) {
  char buffer[1024] = {};
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  std::lock_guard<std::mutex> lock(g_logMutex);
  TestLogCap::Append(g_logLines, buffer);
}

namespace {

void ClearLog() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_logLines.clear();
}

bool LogContains(const std::string& needle) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  return std::any_of(g_logLines.begin(), g_logLines.end(),
                     [&needle](const std::string& line) { return line.find(needle) != std::string::npos; });
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// Returns the value of `name` (case-insensitive) from a raw HTTP request head,
// or "<absent>" when the request did not carry it.
std::string HeaderValue(const std::string& request, const std::string& name) {
  const std::string wanted = Lower(name) + ":";
  size_t lineStart = 0;
  while (lineStart < request.size()) {
    size_t lineEnd = request.find("\r\n", lineStart);
    if (lineEnd == std::string::npos) lineEnd = request.size();
    const std::string line = request.substr(lineStart, lineEnd - lineStart);
    if (Lower(line.substr(0, wanted.size())) == wanted) {
      size_t valueStart = wanted.size();
      while (valueStart < line.size() && line[valueStart] == ' ') ++valueStart;
      return line.substr(valueStart);
    }
    lineStart = lineEnd + 2;
  }
  return "<absent>";
}

// Accepts one connection per script step and answers its upgrade request with
// the scripted reply. Records the Authorization header of every handshake.
class ScriptedUpgradeServer {
 public:
  enum class Reply {
    AcceptThenDrop,  // 101, then close the socket: the client sees a dropped link
    AcceptAndHold,   // 101, keep the socket open until teardown
    Reject401,       // what Nakama answers to an expired or invalid JWT
    Reject503,       // a failure that has nothing to do with the token
  };

  explicit ScriptedUpgradeServer(std::vector<Reply> script) : script_(std::move(script)) {
    WSADATA wsaData = {};
    started_ = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (listener_ == INVALID_SOCKET ||
        bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener_, 8) != 0) {
      return;
    }
    sockaddr_in bound = {};
    int boundLen = sizeof(bound);
    if (getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) return;
    port_ = ntohs(bound.sin_port);
    thread_ = std::thread([this]() { Serve(); });
  }

  ~ScriptedUpgradeServer() {
    if (listener_ != INVALID_SOCKET) closesocket(listener_);
    if (thread_.joinable()) thread_.join();
    for (SOCKET held : held_) closesocket(held);
    if (started_) WSACleanup();
  }

  ScriptedUpgradeServer(const ScriptedUpgradeServer&) = delete;
  ScriptedUpgradeServer& operator=(const ScriptedUpgradeServer&) = delete;

  uint16_t Port() const { return port_; }

  bool WaitForHandshakes(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count]() { return authorizations_.size() >= count; });
  }

  std::vector<std::string> Authorizations() {
    std::lock_guard<std::mutex> lock(mutex_);
    return authorizations_;
  }

 private:
  void Serve() {
    for (Reply reply : script_) {
      SOCKET client = accept(listener_, nullptr, nullptr);
      if (client == INVALID_SOCKET) return;
      const std::string request = ReadRequestHead(client);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        authorizations_.push_back(HeaderValue(request, "Authorization"));
      }
      cv_.notify_all();
      switch (reply) {
        case Reply::AcceptThenDrop:
          SendAll(client, UpgradeResponse(request));
          shutdown(client, SD_BOTH);
          closesocket(client);
          break;
        case Reply::AcceptAndHold:
          SendAll(client, UpgradeResponse(request));
          held_.push_back(client);
          break;
        case Reply::Reject401:
          SendAll(client, "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
          shutdown(client, SD_BOTH);
          closesocket(client);
          break;
        case Reply::Reject503:
          SendAll(client, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
          shutdown(client, SD_BOTH);
          closesocket(client);
          break;
      }
    }
  }

  static std::string ReadRequestHead(SOCKET client) {
    std::string request;
    char chunk[512];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
      const int received = recv(client, chunk, sizeof(chunk), 0);
      if (received <= 0) break;
      request.append(chunk, static_cast<size_t>(received));
    }
    return request;
  }

  static std::string UpgradeResponse(const std::string& request) {
    char accept[29] = {};
    // WebSocketHandshakeKeyGen is in the global namespace in ixwebsocket 11.4.6.
    ::WebSocketHandshakeKeyGen::generate(HeaderValue(request, "Sec-WebSocket-Key"), accept);
    return std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n") +
           "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
  }

  static void SendAll(SOCKET client, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
      const int n = send(client, data.data() + sent, static_cast<int>(data.size() - sent), 0);
      if (n <= 0) return;
      sent += static_cast<size_t>(n);
    }
  }

  std::vector<Reply> script_;
  bool started_ = false;
  SOCKET listener_ = INVALID_SOCKET;
  uint16_t port_ = 0;
  std::thread thread_;
  std::vector<SOCKET> held_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> authorizations_;
};

constexpr std::chrono::milliseconds kHandshakeWait(10000);

std::string UriFor(const ScriptedUpgradeServer& server) {
  return "ws://127.0.0.1:" + std::to_string(server.Port()) + "/nevr";
}

}  // namespace

// The log sink is capped: a loop that logs on every pass ends the process instead of growing the sink until
// memory runs out. The handler is replaced here to observe the overflow.
static size_t g_capOverflowCalls = 0;
static void CountCapOverflow(size_t) { ++g_capOverflowCalls; }

TEST(WebSocketClientAuth, ASpinningLoggerStopsGrowingTheLogSinkAtTheCap) {
  ClearLog();
  g_capOverflowCalls = 0;
  TestLogCap::g_overflowHandler = CountCapOverflow;
  for (size_t i = 0; i < TestLogCap::kMaxLines + 20; ++i) Log(EchoVR::LogLevel::Info, "spin %zu", i);
  // Empty the sink before the default handler comes back (see test_behavioral.cpp).
  size_t held = 0;
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    held = g_logLines.size();
    g_logLines.clear();
  }
  TestLogCap::g_overflowHandler = TestLogCap::EndProcessOnOverflow;
  EXPECT_EQ(held, 10000u);
  EXPECT_EQ(TestLogCap::kMaxLines, 10000u);
  EXPECT_EQ(g_capOverflowCalls, 20u);
}

// The library behaviour the fix depends on, pinned: with nothing to replace it,
// every automatic reconnect presents the header stored at Connect — including
// after the server has said 401 to it.
TEST(WebSocketClientAuth, ReconnectWithoutRefresherReusesTheConnectTimeToken) {
  ClearLog();
  using Reply = ScriptedUpgradeServer::Reply;
  ScriptedUpgradeServer server({Reply::AcceptThenDrop, Reply::Reject401, Reply::AcceptAndHold});
  ASSERT_NE(server.Port(), 0);

  WebSocketClient client;
  const std::string uri = UriFor(server);
  ASSERT_TRUE(client.Connect(uri.c_str(), "token-at-connect"));
  ASSERT_TRUE(server.WaitForHandshakes(3, kHandshakeWait));
  client.DisableReconnection();

  const auto seen = server.Authorizations();
  ASSERT_GE(seen.size(), 3U);
  EXPECT_EQ(seen[0], "Bearer token-at-connect");
  EXPECT_EQ(seen[1], "Bearer token-at-connect");
  EXPECT_EQ(seen[2], "Bearer token-at-connect");
}

// The fix: a 401 on reconnect makes the client mint a token, and the next
// attempt presents it.
TEST(WebSocketClientAuth, Reconnect401MintsAFreshTokenForTheNextAttempt) {
  ClearLog();
  using Reply = ScriptedUpgradeServer::Reply;
  ScriptedUpgradeServer server({Reply::AcceptThenDrop, Reply::Reject401, Reply::AcceptAndHold});
  ASSERT_NE(server.Port(), 0);

  WebSocketClient client;
  std::atomic<int> refreshes{0};
  client.SetBearerTokenRefresher([&refreshes]() {
    ++refreshes;
    return std::string("token-after-401");
  });
  const std::string uri = UriFor(server);
  ASSERT_TRUE(client.Connect(uri.c_str(), "token-at-connect"));
  ASSERT_TRUE(server.WaitForHandshakes(3, kHandshakeWait));
  client.DisableReconnection();

  const auto seen = server.Authorizations();
  ASSERT_GE(seen.size(), 3U);
  EXPECT_EQ(seen[0], "Bearer token-at-connect");
  EXPECT_EQ(seen[1], "Bearer token-at-connect");  // the drop alone does not mint
  EXPECT_EQ(seen[2], "Bearer token-after-401");
  EXPECT_EQ(refreshes.load(), 1);
  EXPECT_TRUE(LogContains("rejected the bearer token (HTTP 401) — re-acquiring"));
  EXPECT_TRUE(LogContains("Bearer token replaced after HTTP 401 refresh_count=1"));
}

// A failure that is not about the token must not call the auth endpoint.
TEST(WebSocketClientAuth, NonAuthFailureDoesNotMintAToken) {
  ClearLog();
  using Reply = ScriptedUpgradeServer::Reply;
  ScriptedUpgradeServer server({Reply::AcceptThenDrop, Reply::Reject503, Reply::AcceptAndHold});
  ASSERT_NE(server.Port(), 0);

  WebSocketClient client;
  std::atomic<int> refreshes{0};
  client.SetBearerTokenRefresher([&refreshes]() {
    ++refreshes;
    return std::string("unexpected");
  });
  const std::string uri = UriFor(server);
  ASSERT_TRUE(client.Connect(uri.c_str(), "token-at-connect"));
  ASSERT_TRUE(server.WaitForHandshakes(3, kHandshakeWait));
  client.DisableReconnection();

  const auto seen = server.Authorizations();
  ASSERT_GE(seen.size(), 3U);
  EXPECT_EQ(seen[2], "Bearer token-at-connect");
  EXPECT_EQ(refreshes.load(), 0);
}

// When minting fails the stored token stays, and the failure is logged loudly.
TEST(WebSocketClientAuth, FailedMintKeepsTheTokenAndLogsTheFailure) {
  ClearLog();
  using Reply = ScriptedUpgradeServer::Reply;
  ScriptedUpgradeServer server({Reply::AcceptThenDrop, Reply::Reject401, Reply::AcceptAndHold});
  ASSERT_NE(server.Port(), 0);

  WebSocketClient client;
  client.SetBearerTokenRefresher([]() { return std::string(); });
  const std::string uri = UriFor(server);
  ASSERT_TRUE(client.Connect(uri.c_str(), "token-at-connect"));
  ASSERT_TRUE(server.WaitForHandshakes(3, kHandshakeWait));
  client.DisableReconnection();

  const auto seen = server.Authorizations();
  ASSERT_GE(seen.size(), 3U);
  EXPECT_EQ(seen[2], "Bearer token-at-connect");
  EXPECT_TRUE(LogContains("Bearer token re-acquisition failed"));
}

// ---------------------------------------------------------------------------
// #114: BearerReconnectAuth, the telemetry socket's equivalent of the above. The same scripted
// server and a bare ix::WebSocket wired the way TelemetryStreamer::Connect wires it.
// ---------------------------------------------------------------------------
namespace {

class BearerSocket {
 public:
  BearerSocket(const std::string& uri, const std::string& token, BearerReconnectAuth::Refresher refresher)
      : auth_("[TEST.BEARER]") {
    ix::initNetSystem();
    ws_.setUrl(uri);
    auth_.Attach(ws_, token, std::move(refresher));
    ws_.enableAutomaticReconnection();
    ws_.setMinWaitBetweenReconnectionRetries(50);
    ws_.setMaxWaitBetweenReconnectionRetries(200);
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Error) auth_.OnError(msg->errorInfo.http_status);
    });
    ws_.start();
  }
  void StopReconnecting() {
    ws_.disableAutomaticReconnection();
    ws_.stop();
  }
  ~BearerSocket() { ws_.stop(); }
  BearerReconnectAuth& Auth() { return auth_; }

 private:
  ix::WebSocket ws_;
  BearerReconnectAuth auth_;
};

}  // namespace

TEST(BearerReconnectAuth, Reconnect401MintsAFreshTokenForTheNextAttempt) {
  ClearLog();
  using Reply = ScriptedUpgradeServer::Reply;
  ScriptedUpgradeServer server({Reply::AcceptThenDrop, Reply::Reject401, Reply::AcceptAndHold});
  ASSERT_NE(server.Port(), 0);

  std::atomic<int> refreshes{0};
  BearerSocket socket(UriFor(server), "token-at-connect", [&refreshes]() {
    ++refreshes;
    return std::string("token-after-401");
  });
  ASSERT_TRUE(server.WaitForHandshakes(3, kHandshakeWait));
  socket.StopReconnecting();

  const auto seen = server.Authorizations();
  ASSERT_GE(seen.size(), 3U);
  EXPECT_EQ(seen[0], "Bearer token-at-connect");
  EXPECT_EQ(seen[1], "Bearer token-at-connect");  // the drop alone does not mint
  EXPECT_EQ(seen[2], "Bearer token-after-401");
  EXPECT_EQ(refreshes.load(), 1);
  EXPECT_EQ(socket.Auth().RefreshCount(), 1U);
  EXPECT_TRUE(LogContains("rejected the bearer token (HTTP 401) — re-acquiring"));
}

// A configured telemetry_token has no refresher: the 401 is logged and the token is presented again.
TEST(BearerReconnectAuth, FixedTokenIsNotReplacedAndTheRejectionIsLogged) {
  ClearLog();
  using Reply = ScriptedUpgradeServer::Reply;
  ScriptedUpgradeServer server({Reply::AcceptThenDrop, Reply::Reject401, Reply::AcceptAndHold});
  ASSERT_NE(server.Port(), 0);

  BearerSocket socket(UriFor(server), "configured-token", nullptr);
  ASSERT_TRUE(server.WaitForHandshakes(3, kHandshakeWait));
  socket.StopReconnecting();

  const auto seen = server.Authorizations();
  ASSERT_GE(seen.size(), 3U);
  EXPECT_EQ(seen[2], "Bearer configured-token");
  EXPECT_EQ(socket.Auth().RefreshCount(), 0U);
  EXPECT_TRUE(LogContains("not refreshable (configured token)"));
}

TEST(BearerReconnectAuth, NonAuthFailureDoesNotMintAToken) {
  ClearLog();
  using Reply = ScriptedUpgradeServer::Reply;
  ScriptedUpgradeServer server({Reply::AcceptThenDrop, Reply::Reject503, Reply::AcceptAndHold});
  ASSERT_NE(server.Port(), 0);

  std::atomic<int> refreshes{0};
  BearerSocket socket(UriFor(server), "token-at-connect", [&refreshes]() {
    ++refreshes;
    return std::string("unexpected");
  });
  ASSERT_TRUE(server.WaitForHandshakes(3, kHandshakeWait));
  socket.StopReconnecting();

  const auto seen = server.Authorizations();
  ASSERT_GE(seen.size(), 3U);
  EXPECT_EQ(seen[2], "Bearer token-at-connect");
  EXPECT_EQ(refreshes.load(), 0);
}

TEST(BearerReconnectAuth, FailedMintKeepsTheTokenAndLogsTheFailure) {
  ClearLog();
  using Reply = ScriptedUpgradeServer::Reply;
  ScriptedUpgradeServer server({Reply::AcceptThenDrop, Reply::Reject401, Reply::AcceptAndHold});
  ASSERT_NE(server.Port(), 0);

  BearerSocket socket(UriFor(server), "token-at-connect", []() { return std::string(); });
  ASSERT_TRUE(server.WaitForHandshakes(3, kHandshakeWait));
  socket.StopReconnecting();

  const auto seen = server.Authorizations();
  ASSERT_GE(seen.size(), 3U);
  EXPECT_EQ(seen[2], "Bearer token-at-connect");
  EXPECT_TRUE(LogContains("bearer token re-acquisition failed"));
}

// #246: a server that rejects even a fresh token must not cost a mint per 401. A bare ix::WebSocket
// that is never started is enough: OnError only needs automatic reconnection enabled.
TEST(BearerReconnectAuth, RepeatedRejectionsMintOncePerInterval) {
  ClearLog();
  ix::WebSocket ws;
  ws.enableAutomaticReconnection();
  std::atomic<int> mints{0};
  BearerReconnectAuth auth("[TEST.BEARER]", std::chrono::hours(1));
  auth.Attach(ws, "t0", [&mints]() {
    ++mints;
    return std::string("t-fresh");
  });
  for (int i = 0; i < 5; ++i) auth.OnError(401);
  EXPECT_EQ(mints.load(), 1);
  EXPECT_EQ(auth.RefreshCount(), 1U);
  EXPECT_TRUE(LogContains("not re-acquiring again within"));
}

// #254: Disconnect() joins ixwebsocket's thread; a 401 arriving during the stop must not start another
// mint (a mint can block 10 s in HTTP). Attach() re-arms for the next Connect.
TEST(BearerReconnectAuth, CancelStopsFurtherMintsUntilTheNextAttach) {
  ix::WebSocket ws;
  ws.enableAutomaticReconnection();
  std::atomic<int> mints{0};
  BearerReconnectAuth auth("[TEST.BEARER]", std::chrono::milliseconds(0));
  const auto refresher = [&mints]() {
    ++mints;
    return std::string("t-fresh");
  };
  auth.Attach(ws, "t0", refresher);
  auth.OnError(401);
  EXPECT_EQ(mints.load(), 1);

  auth.Cancel();
  auth.OnError(401);
  auth.OnError(401);
  EXPECT_EQ(mints.load(), 1) << "no mint after Cancel";
  EXPECT_EQ(auth.RefreshCount(), 1U);

  auth.Attach(ws, "t1", refresher);
  auth.OnError(401);
  EXPECT_EQ(mints.load(), 2) << "Attach re-arms";
}

TEST(BearerReconnectAuth, ZeroIntervalMintsOnEveryRejection) {
  ix::WebSocket ws;
  ws.enableAutomaticReconnection();
  std::atomic<int> mints{0};
  BearerReconnectAuth auth("[TEST.BEARER]", std::chrono::milliseconds(0));
  auth.Attach(ws, "t0", [&mints]() {
    ++mints;
    return std::string("t-fresh");
  });
  for (int i = 0; i < 3; ++i) auth.OnError(401);
  EXPECT_EQ(mints.load(), 3);
}

// #246: the ServerDB refresher, the telemetry refresher and RequestRegistration all end in an unlocked
// write of .credentials.json, so no two mints may overlap.
TEST(SerializedMint, ConcurrentMintsNeverOverlap) {
  std::atomic<int> active{0};
  std::atomic<int> maxActive{0};
  auto mint = [&]() {
    const int now = ++active;
    int seen = maxActive.load();
    while (now > seen && !maxActive.compare_exchange_weak(seen, now)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    --active;
    return std::string("tok");
  };
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&]() { EXPECT_EQ(ServerDbAuth::RunSerializedMint(mint), "tok"); });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(maxActive.load(), 1);
}
