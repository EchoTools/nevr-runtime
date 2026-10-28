#pragma once
// A scripted HTTP server on the loopback interface, for the tests that drive a real libcurl transport (#1156): it accepts one
// connection per script step, records the request (line, Authorization, Content-Type, body) and answers or holds it.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace loopback {

struct Script {
  enum class Kind { Reply, Hold };
  Kind kind = Kind::Reply;
  int status = 200;
  std::string body;
  std::string location;  // a redirect target, when set
};

struct Seen {
  std::string request_line;
  std::string authorization;
  std::string content_type;
  std::string body;
};

inline std::string HeaderValue(const std::string& head, const std::string& name) {
  const std::string key = "\r\n" + name + ": ";
  const std::size_t at = head.find(key);
  if (at == std::string::npos) return "";
  const std::size_t from = at + key.size();
  return head.substr(from, head.find("\r\n", from) - from);
}

class LoopbackServer {
 public:
  explicit LoopbackServer(std::vector<Script> script) : script_(std::move(script)) {
    WSADATA data = {};
    started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener_ == INVALID_SOCKET || bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener_, 8) != 0) {
      return;
    }
    sockaddr_in bound = {};
    int len = sizeof(bound);
    if (getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) return;
    port_ = ntohs(bound.sin_port);
    thread_ = std::thread([this] { Serve(); });
  }
  ~LoopbackServer() {
    if (listener_ != INVALID_SOCKET) closesocket(listener_);
    if (thread_.joinable()) thread_.join();
    for (SOCKET held : held_) closesocket(held);
    if (started_) WSACleanup();
  }
  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  std::string Base() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::vector<Seen> Requests() {
    std::lock_guard<std::mutex> lock(m_);
    return seen_;
  }
  bool WaitForRequests(std::size_t n) {
    std::unique_lock<std::mutex> lock(m_);
    return cv_.wait_for(lock, std::chrono::seconds(10), [&] { return seen_.size() >= n; });
  }

 private:
  void Serve() {
    for (const Script& step : script_) {
      SOCKET client = accept(listener_, nullptr, nullptr);
      if (client == INVALID_SOCKET) return;
      Seen seen = Read(client);
      {
        std::lock_guard<std::mutex> lock(m_);
        seen_.push_back(std::move(seen));
      }
      cv_.notify_all();
      if (step.kind == Script::Kind::Hold) {
        held_.push_back(client);
        continue;
      }
      std::string reply = "HTTP/1.1 " + std::to_string(step.status) + " X\r\nContent-Length: " +
                          std::to_string(step.body.size()) + "\r\nConnection: close\r\n";
      if (!step.location.empty()) reply += "Location: " + step.location + "\r\n";
      reply += "\r\n" + step.body;
      send(client, reply.data(), static_cast<int>(reply.size()), 0);
      shutdown(client, SD_BOTH);
      closesocket(client);
    }
  }
  static Seen Read(SOCKET client) {
    std::string raw;
    char chunk[1024];
    std::size_t headEnd = std::string::npos;
    while ((headEnd = raw.find("\r\n\r\n")) == std::string::npos && raw.size() < 65536) {
      const int n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) break;
      raw.append(chunk, static_cast<std::size_t>(n));
    }
    Seen seen;
    if (headEnd == std::string::npos) return seen;
    const std::string head = raw.substr(0, headEnd);
    seen.request_line = head.substr(0, head.find("\r\n"));
    seen.authorization = HeaderValue(head, "Authorization");
    seen.content_type = HeaderValue(head, "Content-Type");
    const std::size_t want = static_cast<std::size_t>(std::strtoul(HeaderValue(head, "Content-Length").c_str(), nullptr, 10));
    std::string body = raw.substr(headEnd + 4);
    while (body.size() < want) {
      const int n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) break;
      body.append(chunk, static_cast<std::size_t>(n));
    }
    seen.body = body;
    return seen;
  }

  std::vector<Script> script_;
  bool started_ = false;
  SOCKET listener_ = INVALID_SOCKET;
  std::uint16_t port_ = 0;
  std::thread thread_;
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<Seen> seen_;
  std::vector<SOCKET> held_;
};

}  // namespace loopback
