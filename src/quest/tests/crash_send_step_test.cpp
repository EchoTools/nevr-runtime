// Host test for the Quest crash report POST (integration/crash_send_step.h, CurlCrashPost): what the sentinel's libcurl client sends
// to a loopback server and what it reports back, and that the production setting (no context) refuses plain http. The sender's own
// decisions (what to send, when to retry) are core/crash_send.h, tested in crash_send_test.

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "quest/integration/crash_send_step.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/test_check.h"

// The sentinel's log is not part of this test: CurlCrashPost writes none, and the file under test only reaches it from the send thread.
namespace sentinel {
void LogFields(LogLevel, const char*, std::initializer_list<LogField>) {}
}  // namespace sentinel

namespace {

namespace step = nevr_quest::integration;

constexpr long kConnectMs = 2000;
constexpr long kTotalMs = 4000;
constexpr long kShortTotalMs = 1000;
constexpr long kOkStatus = 200;
constexpr long kUnavailableStatus = 503;
constexpr int kBacklog = 4;
constexpr int kPollMs = 20;
constexpr int kReadSeconds = 5;
constexpr std::size_t kReadChunk = 4096;
constexpr std::chrono::milliseconds kHoldStep{20};
constexpr std::chrono::milliseconds kTooLong{1500};  // over the 1 s total, under the 2 s connect budget
constexpr std::chrono::milliseconds kAtLeastOneSecond{900};

std::string Response(long status, const std::string& body) {
  return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
         body;
}

// A loopback HTTP server on its own thread: each connection is read to the end of its request (head and Content-Length body),
// recorded, and answered with `reply`; a held server never answers until it is destroyed.
class Loopback {
 public:
  Loopback(std::string reply, bool hold) : reply_(std::move(reply)), hold_(hold) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    const bool ready = listen_fd_ >= 0 && ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                       ::listen(listen_fd_, kBacklog) == 0 && ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0;
    QCHECK(ready);
    port_ = ready ? ntohs(addr.sin_port) : 0;
    thread_ = std::thread([this] { Run(); });
  }
  ~Loopback() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) ::close(listen_fd_);
  }
  Loopback(const Loopback&) = delete;
  Loopback& operator=(const Loopback&) = delete;

  int Port() const { return port_; }
  std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1/crash"; }
  std::vector<std::string> Requests() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

 private:
  void Run() {
    while (!stop_) {
      pollfd pfd{listen_fd_, POLLIN, 0};
      if (::poll(&pfd, 1, kPollMs) <= 0) continue;
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd >= 0) Serve(fd);
    }
  }

  void Serve(int fd) {
    timeval timeout{kReadSeconds, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string data;
    char buf[kReadChunk];
    std::size_t needed = std::string::npos;
    for (;;) {
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) break;
      data.append(buf, static_cast<std::size_t>(n));
      if (needed == std::string::npos) {
        const std::size_t headEnd = data.find("\r\n\r\n");
        const std::size_t lengthAt = data.find("Content-Length: ");
        if (headEnd != std::string::npos && lengthAt != std::string::npos) {
          needed = headEnd + std::string("\r\n\r\n").size() + std::stoul(data.substr(lengthAt + std::string("Content-Length: ").size()));
        }
      }
      if (needed != std::string::npos && data.size() >= needed) break;
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(data);
    }
    if (hold_) {
      while (!stop_) std::this_thread::sleep_for(kHoldStep);
    } else {
      ::send(fd, reply_.data(), reply_.size(), MSG_NOSIGNAL);
    }
    ::close(fd);
  }

  std::string reply_;
  bool hold_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  mutable std::mutex mutex_;
  std::vector<std::string> requests_;
  std::thread thread_;
};

nevr_crash_send::Timeouts Timeouts(long totalMs) {
  nevr_crash_send::Timeouts timeouts;
  timeouts.connectMs = kConnectMs;
  timeouts.totalMs = totalMs;
  return timeouts;
}

void PostsTheBodyAsJsonAndReportsTheStatus() {
  Loopback server(Response(kOkStatus, "{}"), false);
  bool allowPlain = true;
  const std::string body = "{\"build\":\"x\"}";
  const nevr_crash_send::PostResult result = step::CurlCrashPost(&allowPlain, server.Url(), body, Timeouts(kTotalMs));
  QCHECK(result.transportOk);
  QCHECK(result.status == kOkStatus);
  const std::vector<std::string> requests = server.Requests();
  QCHECK(requests.size() == 1);
  if (requests.size() == 1) {
    QCHECK(requests[0].rfind("POST /v1/crash HTTP/1.1", 0) == 0);
    QCHECK(requests[0].find("Content-Type: application/json") != std::string::npos);
    QCHECK(requests[0].size() >= body.size() && requests[0].compare(requests[0].size() - body.size(), body.size(), body) == 0);
    QCHECK(requests[0].find("Authorization") == std::string::npos);  // the report carries no credential
  }
}

void AnErrorStatusIsReportedNotHidden() {
  Loopback server(Response(kUnavailableStatus, ""), false);
  bool allowPlain = true;
  const nevr_crash_send::PostResult result = step::CurlCrashPost(&allowPlain, server.Url(), "{}", Timeouts(kTotalMs));
  QCHECK(result.transportOk);  // a status was obtained: the sender, not this POST, decides what a 503 means
  QCHECK(result.status == kUnavailableStatus);
}

void ADeadPortIsATransportFailureWithTheCurlCode() {
  int port = 0;
  {
    Loopback closed(Response(kOkStatus, ""), false);
    port = closed.Port();
  }  // the listener is gone: nothing answers on that port
  bool allowPlain = true;
  const nevr_crash_send::PostResult result =
      step::CurlCrashPost(&allowPlain, "http://127.0.0.1:" + std::to_string(port) + "/v1/crash", "{}", Timeouts(kTotalMs));
  QCHECK(!result.transportOk);
  QCHECK(result.status == 0);
  QCHECK(result.errorCode == CURLE_COULDNT_CONNECT);
}

void AnUnansweredRequestEndsAtTheTotalTimeout() {
  Loopback server("", true);
  bool allowPlain = true;
  const auto begin = std::chrono::steady_clock::now();
  const nevr_crash_send::PostResult result = step::CurlCrashPost(&allowPlain, server.Url(), "{}", Timeouts(kShortTotalMs));
  const auto took = std::chrono::steady_clock::now() - begin;
  QCHECK(!result.transportOk);
  QCHECK(result.errorCode == CURLE_OPERATION_TIMEDOUT);
  QCHECK(took >= kAtLeastOneSecond);  // the 1000 ms budget became one second, not zero
  QCHECK(took < kTooLong);
}

void TheSentinelsSettingRefusesPlainHttp() {
  Loopback server(Response(kOkStatus, ""), false);
  const nevr_crash_send::PostResult noContext = step::CurlCrashPost(nullptr, server.Url(), "{}", Timeouts(kTotalMs));
  QCHECK(!noContext.transportOk);
  QCHECK(noContext.errorCode != 0);
  bool allowPlain = false;
  const nevr_crash_send::PostResult falseContext = step::CurlCrashPost(&allowPlain, server.Url(), "{}", Timeouts(kTotalMs));
  QCHECK(!falseContext.transportOk);
  QCHECK(server.Requests().empty());  // nothing reached the server: the report never leaves in clear
}

}  // namespace

int main() {
  PostsTheBodyAsJsonAndReportsTheStatus();
  AnErrorStatusIsReportedNotHidden();
  ADeadPortIsATransportFailureWithTheCurlCode();
  AnUnansweredRequestEndsAtTheTotalTimeout();
  TheSentinelsSettingRefusesPlainHttp();
  return quest_test::Failures() == 0 ? 0 : 1;
}
