#pragma once

// Bearer-token upgrade header that survives ixwebsocket's automatic reconnect (#114; the ServerDB
// socket got the same fix in #39 inside WebSocketClient).
//
// ixwebsocket reconnects by calling WebSocket::connect() again, which re-sends the extra headers
// stored at the first connect. A JWT that expired in between is rejected at the upgrade with HTTP
// 401 on every attempt, forever. Attach() stores the token as the Authorization header; OnError()
// is fed each Error message's HTTP status and, on 401 only, mints a fresh token through the
// refresher and stores that one for the next attempt. A network failure or a 5xx is not a token
// problem and never calls the refresher. At most one mint is attempted per minRefreshInterval: a
// server that rejects even a fresh token would otherwise cost a refresh POST, a credentials write
// and log lines on every reconnect attempt, forever.
//
// Thread: OnError runs on ixwebsocket's own thread (an Error message is emitted only from its
// connection check), the thread whose next connect() reads the stored headers, so writing the
// header there orders it before that read. The refresher may block on HTTP.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace ix {
class WebSocket;
}

class BearerReconnectAuth {
 public:
  // Mints a fresh token; returns "" when none could be acquired.
  using Refresher = std::function<std::string()>;

  // `logTag` prefixes the log lines, e.g. "[NEVR.TELEMETRY]".
  static constexpr std::chrono::milliseconds kDefaultMinRefreshInterval{30000};

  explicit BearerReconnectAuth(std::string logTag,
                               std::chrono::milliseconds minRefreshInterval = kDefaultMinRefreshInterval)
      : logTag_(std::move(logTag)), minRefreshInterval_(minRefreshInterval) {}

  // Stores `token` as the Authorization header of `ws`. An empty token sends no header and turns
  // OnError into a no-op. A null `refresher` means the token is fixed (configured by the operator):
  // a 401 is logged and the same token is presented again.
  void Attach(ix::WebSocket& ws, const std::string& token, Refresher refresher);

  // Call from the Error message handler with errorInfo.http_status.
  void OnError(int httpStatus);

  uint32_t RefreshCount() const { return refreshCount_.load(); }

 private:
  const std::string logTag_;
  const std::chrono::milliseconds minRefreshInterval_;
  bool attempted_ = false;
  std::chrono::steady_clock::time_point lastAttempt_;
  ix::WebSocket* ws_ = nullptr;
  std::mutex mutex_;
  std::string token_;
  Refresher refresher_;
  std::atomic<uint32_t> refreshCount_{0};
};
