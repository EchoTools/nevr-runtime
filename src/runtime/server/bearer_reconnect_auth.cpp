#include "runtime/server/bearer_reconnect_auth.h"

#include <ixwebsocket/IXWebSocket.h>

#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "abi/echovr.h"

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

void BearerReconnectAuth::Attach(ix::WebSocket& ws, const std::string& token, Refresher refresher) {
  std::lock_guard<std::mutex> lock(mutex_);
  ws_ = &ws;
  token_ = token;
  refresher_ = std::move(refresher);
  if (token_.empty()) return;
  ix::WebSocketHttpHeaders headers;
  headers["Authorization"] = "Bearer " + token_;
  ws.setExtraHeaders(headers);
}

void BearerReconnectAuth::OnError(int httpStatus) {
  if (httpStatus != 401) return;

  Refresher refresher;
  ix::WebSocket* ws = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (token_.empty()) return;  // no bearer header was sent: the 401 is about other credentials
    refresher = refresher_;
    ws = ws_;
  }
  if (!refresher) {
    Log(EchoVR::LogLevel::Warning,
        "%s rejected the bearer token (HTTP 401) and it is not refreshable (configured token) "
        "— every reconnect presents the same token",
        logTag_.c_str());
    return;
  }
  if (ws == nullptr || !ws->isAutomaticReconnectionEnabled()) {
    Log(EchoVR::LogLevel::Info,
        "%s rejected the bearer token (HTTP 401) after reconnection was disabled — not re-acquiring",
        logTag_.c_str());
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (attempted_ && now - lastAttempt_ < minRefreshInterval_) {
      const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAttempt_).count();
      Log(EchoVR::LogLevel::Warning,
          "%s rejected the bearer token (HTTP 401) %lld ms after the last re-acquisition — not "
          "re-acquiring again within %lld ms",
          logTag_.c_str(), static_cast<long long>(sinceMs), static_cast<long long>(minRefreshInterval_.count()));
      return;
    }
    attempted_ = true;
    lastAttempt_ = now;
  }

  Log(EchoVR::LogLevel::Warning,
      "%s rejected the bearer token (HTTP 401) — re-acquiring before the next reconnect attempt",
      logTag_.c_str());
  const std::string fresh = refresher();
  if (fresh.empty()) {
    Log(EchoVR::LogLevel::Error,
        "%s bearer token re-acquisition failed — the next reconnect attempt presents the rejected token",
        logTag_.c_str());
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    token_ = fresh;
  }
  ix::WebSocketHttpHeaders headers;
  headers["Authorization"] = "Bearer " + fresh;
  ws->setExtraHeaders(headers);
  const uint32_t count = ++refreshCount_;
  Log(EchoVR::LogLevel::Info, "%s bearer token replaced after HTTP 401 refresh_count=%u", logTag_.c_str(), count);
}
