#include "runtime/server/websocket_client.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <cstring>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "abi/echovr.h"
#include "runtime/server/websocket_frame.h"
#include "runtime/log/url_diagnostics.h"
#include "runtime/log/security_diagnostics.h"

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

static uint32_t s_wsReconnectCount = 0;
static bool s_wsHasConnectedOnce = false;

WebSocketClient::WebSocketClient() : webSocket_(std::make_unique<ix::WebSocket>()) {
  // Initialize network system (required on Windows)
  // Note: Using static variable for one-time initialization across all instances
  static bool netSystemInitialized_ = false;
  if (!netSystemInitialized_) {
    ix::initNetSystem();
    netSystemInitialized_ = true;
  }

  // Initialize thread synchronization for received messages
  InitializeCriticalSection(&receivedMessagesMutex_);

  // Set up the message callback
  webSocket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) { OnMessage(msg); });
}

WebSocketClient::~WebSocketClient() {
  Disconnect();
  DeleteCriticalSection(&receivedMessagesMutex_);
}

BOOL WebSocketClient::Connect(const CHAR* uri, const std::string& bearerToken) {
  if (!uri || strlen(uri) == 0) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SERVERDB] Invalid URI provided for connection");
    return FALSE;
  }

  const std::string diagnostic = LogDiagnostics::FormatRedactedUrlDiagnostic("[NEVR.SERVERDB] Connecting to ServerDB at ", uri);
  Log(EchoVR::LogLevel::Info, "%s", diagnostic.c_str());

  // Set the URL
  webSocket_->setUrl(std::string(uri));

  // Attach Bearer token on WebSocket upgrade request
  if (!bearerToken.empty()) {
    ApplyBearerToken(bearerToken);
    Log(EchoVR::LogLevel::Debug, "[NEVR.SERVERDB] Using Bearer auth token");
  }

  // Start the connection (non-blocking)
  webSocket_->start();

  return TRUE;
}

VOID WebSocketClient::Disconnect() {
  if (webSocket_) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SERVERDB] Disconnecting from ServerDB");
    webSocket_->stop();
    connected_.store(false);
  }
}

BOOL WebSocketClient::Send(EchoVR::SymbolId msgId, const VOID* data, UINT64 size) {
  return SendWithStatus(msgId, data, size) != WebSocketSendStatus::Rejected;
}

WebSocketSendStatus WebSocketClient::SendWithStatus(EchoVR::SymbolId msgId, const VOID* data, UINT64 size) {
  if (size > 1024 * 1024) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SERVERDB] Rejecting oversized send (msgId: 0x%llX, size: %llu)", msgId, size);
    return WebSocketSendStatus::Rejected;
  }
  const UINT64 MAGIC = 0xBB8CE7A278BB40F6;
  std::vector<uint8_t> messageBuffer(sizeof(UINT64) + sizeof(EchoVR::SymbolId) + sizeof(UINT64) + static_cast<size_t>(size));

  memcpy(messageBuffer.data(), &MAGIC, sizeof(UINT64));
  memcpy(messageBuffer.data() + sizeof(UINT64), &msgId, sizeof(EchoVR::SymbolId));
  memcpy(messageBuffer.data() + sizeof(UINT64) + sizeof(EchoVR::SymbolId), &size, sizeof(UINT64));

  if (size > 0 && data != nullptr) {
    memcpy(messageBuffer.data() + sizeof(UINT64) + sizeof(EchoVR::SymbolId) + sizeof(UINT64), data, size);
  }

  std::string message(messageBuffer.begin(), messageBuffer.end());

  if (!connected_.load(std::memory_order_relaxed)) {
    EnterCriticalSection(&receivedMessagesMutex_);
    if (pendingMessages_.size() >= 256) {
      LeaveCriticalSection(&receivedMessagesMutex_);
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.SERVERDB] Pending message queue full (256) — dropping message (msgId: 0x%llX)", msgId);
      return WebSocketSendStatus::Rejected;
    }
    pendingMessages_.push_back(message);
    LeaveCriticalSection(&receivedMessagesMutex_);
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.SERVERDB] Queued message (msgId: 0x%llX, size: %llu bytes, payload: %llu bytes) - will send when connected",
        msgId, size, size);
    return WebSocketSendStatus::Queued;
  }

#ifdef NEVR_TEST_HOOKS
  if (testTransportHandler_) {
    if (!testTransportHandler_(message)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.SERVERDB] Failed to send message (msgId: 0x%llX)", msgId);
      return WebSocketSendStatus::Rejected;
    }
    return WebSocketSendStatus::Sent;
  }
#endif

  auto result = webSocket_->send(message, true);

  if (!result.success) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SERVERDB] Failed to send message (msgId: 0x%llX)", msgId);
    return WebSocketSendStatus::Rejected;
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.SERVERDB] Sent message (msgId: 0x%llX, size: %llu bytes, total: %zu bytes)", msgId,
      size, messageBuffer.size());

  return WebSocketSendStatus::Sent;
}

size_t WebSocketClient::DiscardPendingMessages() {
  EnterCriticalSection(&receivedMessagesMutex_);
  const size_t discarded = pendingMessages_.size();
  pendingMessages_.clear();
  LeaveCriticalSection(&receivedMessagesMutex_);
  return discarded;
}

VOID WebSocketClient::SetMessageHandler(MessageCallback callback) { messageCallback_ = callback; }

VOID WebSocketClient::SetConnectionHandler(ConnectionCallback callback) { connectionCallback_ = callback; }

VOID WebSocketClient::SetBearerTokenRefresher(BearerTokenRefresher refresher) {
  std::lock_guard<std::mutex> lock(bearerTokenMutex_);
  bearerTokenRefresher_ = std::move(refresher);
}

VOID WebSocketClient::ApplyBearerToken(const std::string& token) {
  std::lock_guard<std::mutex> lock(bearerTokenMutex_);
  bearerToken_ = token;
  ix::WebSocketHttpHeaders headers;
  headers["Authorization"] = "Bearer " + token;
  webSocket_->setExtraHeaders(headers);
}

// Issue #39. ixwebsocket 11.4.6 reconnects by calling WebSocket::connect() again,
// which copies the stored _extraHeaders (IXWebSocket.cpp:210) — the header set at
// Connect. Nakama rejects an expired JWT at the upgrade with 401 (parseToken uses
// jwt.WithExpirationRequired), so without this every reconnect after the token's
// TTL fails the same way, forever.
//
// Only 401 triggers it. A network failure or a 5xx is not a token problem, and
// minting on those would call the auth endpoint on every network blip.
//
// Thread: an Error message is emitted only by WebSocket::checkConnection
// (IXWebSocket.cpp:362), on ixwebsocket's own thread — the thread whose next
// connect() reads _extraHeaders without the config mutex. Writing the header
// here orders the write before that read. A Close message can also arrive on the
// game thread (a failed send closes the socket), so this must not move there.
VOID WebSocketClient::RefreshBearerTokenAfterRejection() {
  BearerTokenRefresher refresher;
  bool usingBearer = false;
  {
    std::lock_guard<std::mutex> lock(bearerTokenMutex_);
    refresher = bearerTokenRefresher_;
    usingBearer = !bearerToken_.empty();
  }
  // No bearer header was sent, so the 401 is about other credentials (the
  // legacy url-param route); the Error line above already records it.
  if (!usingBearer) return;

  if (!refresher) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.SERVERDB] ServerDB rejected the bearer token (HTTP 401) and no token refresher is set "
        "— every reconnect presents the same token");
    return;
  }
  if (!webSocket_->isAutomaticReconnectionEnabled()) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SERVERDB] ServerDB rejected the bearer token (HTTP 401) after reconnection was disabled "
        "— not re-acquiring");
    return;
  }

  Log(EchoVR::LogLevel::Warning,
      "[NEVR.SERVERDB] ServerDB rejected the bearer token (HTTP 401) — re-acquiring before the next reconnect attempt");
  const std::string fresh = refresher();
  if (fresh.empty()) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.SERVERDB] Bearer token re-acquisition failed — the next reconnect attempt presents the rejected token");
    return;
  }
  ApplyBearerToken(fresh);
  const uint32_t count = ++bearerTokenRefreshCount_;
  Log(EchoVR::LogLevel::Info, "[NEVR.SERVERDB] Bearer token replaced after HTTP 401 refresh_count=%u", count);
}

BOOL WebSocketClient::IsConnected() const { return connected_.load(std::memory_order_relaxed); }

VOID WebSocketClient::OnMessage(const ix::WebSocketMessagePtr& msg) {
  switch (msg->type) {
    case ix::WebSocketMessageType::Open:
      if (s_wsHasConnectedOnce) ++s_wsReconnectCount;
      s_wsHasConnectedOnce = true;
      Log(EchoVR::LogLevel::Info, "[NEVR.SERVERDB] Connected to ServerDB reconnect_count=%u", s_wsReconnectCount);
      connected_.store(true);
      FlushPendingMessages();
      if (connectionCallback_) {
        connectionCallback_(TRUE);
      }
      break;

    case ix::WebSocketMessageType::Close:
      {
        const std::string diagnostic = LogDiagnostics::FormatWebSocketCloseDiagnostic(
            "[NEVR.SERVERDB] Disconnected from ServerDB ", msg->closeInfo.code, s_wsReconnectCount);
        Log(EchoVR::LogLevel::Info, "%s", diagnostic.c_str());
      }
      connected_.store(false);
      break;

    case ix::WebSocketMessageType::Error:
      {
        const std::string diagnostic = LogDiagnostics::FormatWebSocketErrorDiagnostic(
            "[NEVR.SERVERDB] Connection error: ", msg->errorInfo.http_status, msg->errorInfo.retries,
            s_wsReconnectCount);
        Log(EchoVR::LogLevel::Error, "%s", diagnostic.c_str());
      }
      connected_.store(false);
      if (msg->errorInfo.http_status == 401) RefreshBearerTokenAfterRejection();
      break;

    case ix::WebSocketMessageType::Message:
      if (msg->binary) {
        const std::string& payload = msg->str;
        size_t queueCapacity = 0;
        EnterCriticalSection(&receivedMessagesMutex_);
        if (receivedMessages_.size() < 1024) queueCapacity = 1024 - receivedMessages_.size();
        LeaveCriticalSection(&receivedMessagesMutex_);

        auto parsed = nevr_game_server::ParseServerDbFrame(payload, queueCapacity);
        bool queueFull = false;
        EnterCriticalSection(&receivedMessagesMutex_);
        for (auto& received : parsed.messages) {
          if (receivedMessages_.size() >= 1024) {
            queueFull = true;
            break;
          }
          receivedMessages_.push_back(std::move(received));
        }
        LeaveCriticalSection(&receivedMessagesMutex_);

        switch (parsed.status) {
          case nevr_game_server::WebSocketFrameStatus::Complete:
            break;
          case nevr_game_server::WebSocketFrameStatus::TooShort:
            Log(EchoVR::LogLevel::Warning, "[NEVR.SERVERDB] Received malformed binary message (too short: %zu bytes)",
                payload.size());
            break;
          case nevr_game_server::WebSocketFrameStatus::InvalidMagic:
            Log(EchoVR::LogLevel::Warning, "[NEVR.SERVERDB] Received binary frame with invalid magic at offset %zu",
                parsed.errorOffset);
            break;
          case nevr_game_server::WebSocketFrameStatus::TruncatedHeader:
            Log(EchoVR::LogLevel::Warning, "[NEVR.SERVERDB] Truncated message header at offset %zu (%zu bytes remain)",
                parsed.errorOffset, parsed.remainingLength);
            break;
          case nevr_game_server::WebSocketFrameStatus::TruncatedPayload:
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.SERVERDB] Message length exceeds frame (msgId: 0x%llX, length: %llu, remaining: %zu)",
                parsed.errorMessageId, static_cast<unsigned long long>(parsed.declaredLength),
                parsed.remainingLength);
            break;
          case nevr_game_server::WebSocketFrameStatus::OversizedMessage:
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.SERVERDB] Dropped oversized message (msgId: 0x%llX, size: %llu bytes)",
                parsed.errorMessageId, static_cast<unsigned long long>(parsed.declaredLength));
            break;
          case nevr_game_server::WebSocketFrameStatus::QueueLimit:
            queueFull = true;
            break;
        }
        if (queueFull) {
          Log(EchoVR::LogLevel::Warning, "[NEVR.SERVERDB] Receive queue full (1024) — remaining frame messages dropped");
        }
        if (parsed.messages.size() > 1) {
          Log(EchoVR::LogLevel::Debug, "[NEVR.SERVERDB] Parsed %zu messages from single frame (%zu bytes)",
              parsed.messages.size(), payload.size());
        }
      } else {
        Log(EchoVR::LogLevel::Warning, "[NEVR.SERVERDB] Received unexpected text message (%zu bytes)",
            msg->str.size());
      }
      break;

    case ix::WebSocketMessageType::Ping:
    case ix::WebSocketMessageType::Pong:
      // Handled automatically by ixwebsocket
      break;

    case ix::WebSocketMessageType::Fragment:
      break;
  }
}

VOID WebSocketClient::FlushPendingMessages() {
  std::vector<std::string> toSend;

  EnterCriticalSection(&receivedMessagesMutex_);
  toSend.swap(pendingMessages_);
  LeaveCriticalSection(&receivedMessagesMutex_);

  if (toSend.empty()) {
    return;
  }

  Log(EchoVR::LogLevel::Debug, "[NEVR.SERVERDB] Flushing %zu pending messages", toSend.size());

  for (const auto& message : toSend) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.SERVERDB] Sending pending message: size=%zu bytes", message.size());
    auto result = webSocket_->send(message, true);
    if (!result.success) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.SERVERDB] Failed to send pending message");
    } else {
      Log(EchoVR::LogLevel::Debug, "[NEVR.SERVERDB] Successfully sent pending message");
    }
  }
}

VOID WebSocketClient::ProcessReceivedMessages() {
  std::vector<nevr_game_server::ReceivedWebSocketMessage> messagesToProcess;

  EnterCriticalSection(&receivedMessagesMutex_);
  messagesToProcess.swap(receivedMessages_);
  LeaveCriticalSection(&receivedMessagesMutex_);

  // Non-const iteration: messagesToProcess is this function's own copy and is
  // discarded after dispatch, so handing the handler a writable payload is honest.
  for (auto& msg : messagesToProcess) {
    if (messageCallback_) {
      VOID* data = msg.payload.empty() ? nullptr : msg.payload.data();
      messageCallback_(msg.msgId, data, msg.payload.size());
    }
  }
}

VOID WebSocketClient::DisableReconnection() {
  if (webSocket_) webSocket_->disableAutomaticReconnection();
}

#ifdef NEVR_TEST_HOOKS
void WebSocketClient::TestSetConnected(bool connected) { connected_.store(connected); }

std::vector<std::string> WebSocketClient::TestCopyPendingMessages() {
  EnterCriticalSection(&receivedMessagesMutex_);
  const std::vector<std::string> messages = pendingMessages_;
  LeaveCriticalSection(&receivedMessagesMutex_);
  return messages;
}

void WebSocketClient::TestSetTransportHandler(std::function<bool(const std::string&)> handler) {
  testTransportHandler_ = std::move(handler);
}

void WebSocketClient::TestEnqueueReceivedMessage(nevr_game_server::ReceivedWebSocketMessage message) {
  EnterCriticalSection(&receivedMessagesMutex_);
  receivedMessages_.push_back(std::move(message));
  LeaveCriticalSection(&receivedMessagesMutex_);
}
#endif
