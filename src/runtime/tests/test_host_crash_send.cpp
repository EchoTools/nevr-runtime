// The host's POST for the crash report send (runtime/host/crash_send_winhttp.cpp, #1689): WinHTTP loaded by name,
// against a loopback server this test runs. The unit under test is the one BugSplat64.dll carries (nevr_host_core).

#include "runtime/host/crash_send_winhttp.h"

#include <gtest/gtest.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

// One connection, one request: reads the head and the body, answers `status`, closes.
class OneShotServer {
 public:
  explicit OneShotServer(int status) : status_(status) {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int length = sizeof(address);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    ::listen(listener_, 1);
    thread_ = std::thread([this] { Serve(); });
  }
  ~OneShotServer() {
    ::closesocket(listener_);  // wakes an accept that never came
    if (thread_.joinable()) thread_.join();
    WSACleanup();
  }
  int port() const { return port_; }
  std::string request() const { return request_; }

 private:
  void Serve() {
    SOCKET client = ::accept(listener_, nullptr, nullptr);
    if (client == INVALID_SOCKET) return;
    std::string data;
    char buffer[4096];
    std::size_t bodyAt = std::string::npos;
    std::size_t want = 0;
    for (;;) {
      const int n = ::recv(client, buffer, sizeof(buffer), 0);
      if (n <= 0) break;
      data.append(buffer, static_cast<std::size_t>(n));
      if (bodyAt == std::string::npos) {
        bodyAt = data.find("\r\n\r\n");
        if (bodyAt != std::string::npos) {
          bodyAt += 4;
          const std::size_t at = data.find("Content-Length: ");
          want = at == std::string::npos ? 0 : static_cast<std::size_t>(std::stoul(data.substr(at + 16)));
        }
      }
      if (bodyAt != std::string::npos && data.size() >= bodyAt + want) break;
    }
    request_ = data;
    const std::string reply = "HTTP/1.1 " + std::to_string(status_) + " X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    ::send(client, reply.data(), static_cast<int>(reply.size()), 0);
    ::closesocket(client);
  }

  int status_;
  SOCKET listener_ = INVALID_SOCKET;
  int port_ = 0;
  std::string request_;
  std::thread thread_;
};

nevr_host::WinHttpPostContext PlainHttp() {
  nevr_host::WinHttpPostContext context;
  context.allowPlainHttp = true;
  return context;
}

}  // namespace

TEST(HostCrashSend, PostsTheBodyAsJsonAndReturnsTheStatus) {
  OneShotServer server(201);
  nevr_host::WinHttpPostContext context = PlainHttp();
  const std::string body = "{\"schema\":1}";
  const auto result = nevr_host::WinHttpPost(&context, "http://127.0.0.1:" + std::to_string(server.port()) + "/api/v3/crash-reports",
                                             body, {3000, 6000});
  ASSERT_TRUE(result.transportOk) << "error " << result.errorCode;
  EXPECT_EQ(result.status, 201);
  const std::string request = server.request();
  EXPECT_EQ(request.rfind("POST /api/v3/crash-reports HTTP/1.1", 0), 0u) << request;
  EXPECT_NE(request.find("Content-Type: application/json"), std::string::npos);
  EXPECT_EQ(request.substr(request.size() - body.size()), body);
  EXPECT_EQ(request.find("Cookie"), std::string::npos);
  EXPECT_EQ(request.find("Authorization"), std::string::npos);
}

TEST(HostCrashSend, AnErrorStatusIsAnAnswerNotAFailure) {
  OneShotServer server(503);
  nevr_host::WinHttpPostContext context = PlainHttp();
  const auto result = nevr_host::WinHttpPost(&context, "http://127.0.0.1:" + std::to_string(server.port()) + "/x", "{}", {3000, 6000});
  ASSERT_TRUE(result.transportOk);
  EXPECT_EQ(result.status, 503);
}

TEST(HostCrashSend, ARedirectIsNotFollowed) {
  OneShotServer server(302);
  nevr_host::WinHttpPostContext context = PlainHttp();
  const auto result = nevr_host::WinHttpPost(&context, "http://127.0.0.1:" + std::to_string(server.port()) + "/x", "{}", {3000, 6000});
  ASSERT_TRUE(result.transportOk);
  EXPECT_EQ(result.status, 302);
}

TEST(HostCrashSend, NothingListeningEndsWithinTheTimeoutWithAnErrorCode) {
  int port = 0;
  {
    OneShotServer server(200);  // takes a port, then gives it back
    port = server.port();
  }
  nevr_host::WinHttpPostContext context = PlainHttp();
  const auto started = std::chrono::steady_clock::now();
  const auto result = nevr_host::WinHttpPost(&context, "http://127.0.0.1:" + std::to_string(port) + "/x", "{}", {1000, 2000});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_FALSE(result.transportOk);
  EXPECT_NE(result.errorCode, 0);
  EXPECT_LT(elapsed, std::chrono::seconds(8));
}

TEST(HostCrashSend, PlainHttpIsRefusedWithoutTheTestContext) {
  OneShotServer server(201);
  const auto result = nevr_host::WinHttpPost(nullptr, "http://127.0.0.1:" + std::to_string(server.port()) + "/x", "{}", {3000, 6000});
  EXPECT_FALSE(result.transportOk);
  EXPECT_EQ(result.errorCode, static_cast<long>(ERROR_INVALID_PARAMETER));
  EXPECT_TRUE(server.request().empty());  // it never connected
}

TEST(HostCrashSend, AMalformedUrlIsRefused) {
  for (const char* url : {"", "https://", "https:///x", "https://user@host/x", "https://host:0/x", "https://host:99999/x", "ftp://host/x"}) {
    const auto result = nevr_host::WinHttpPost(nullptr, url, "{}", {1000, 2000});
    EXPECT_FALSE(result.transportOk) << url;
    EXPECT_EQ(result.errorCode, static_cast<long>(ERROR_INVALID_PARAMETER)) << url;
  }
}
