#include "runtime/server/websocket_client.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "abi/echovr.h"
#include "runtime/server/websocket_frame.h"
#include "runtime/server/url_diagnostics.h"

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

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
    Log(EchoVR::LogLevel::Error, "[WEBSOCKET] Invalid URI provided for connection");
    return FALSE;
  }

  const std::string diagnosticUri = GameServer::RedactUrlForDiagnostics(uri);
  Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Connecting to ServerDB at %s", diagnosticUri.c_str());

  // Set the URL
  webSocket_->setUrl(std::string(uri));

  // Attach Bearer token on WebSocket upgrade request
  if (!bearerToken.empty()) {
    ix::WebSocketHttpHeaders headers;
    headers["Authorization"] = "Bearer " + bearerToken;
    webSocket_->setExtraHeaders(headers);
    Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Using Bearer auth token");
  }

  // Start the connection (non-blocking)
  webSocket_->start();

  return TRUE;
}

VOID WebSocketClient::Disconnect() {
  if (webSocket_) {
    Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Disconnecting from ServerDB");
    webSocket_->stop();
    connected_.store(false);
  }
}

BOOL WebSocketClient::Send(EchoVR::SymbolId msgId, const VOID* data, UINT64 size) {
  return SendWithStatus(msgId, data, size) != WebSocketSendStatus::Rejected;
}

WebSocketSendStatus WebSocketClient::SendWithStatus(EchoVR::SymbolId msgId, const VOID* data, UINT64 size) {
  if (size > 1024 * 1024) {
    Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Rejecting oversized send (msgId: 0x%llX, size: %llu)", msgId, size);
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
          "[WEBSOCKET] Pending message queue full (256) — dropping message (msgId: 0x%llX)", msgId);
      return WebSocketSendStatus::Rejected;
    }
    pendingMessages_.push_back(message);
    LeaveCriticalSection(&receivedMessagesMutex_);
    Log(EchoVR::LogLevel::Debug,
        "[WEBSOCKET] Queued message (msgId: 0x%llX, size: %llu bytes, payload: %llu bytes) - will send when connected",
        msgId, size, size);
    return WebSocketSendStatus::Queued;
  }

#ifdef NEVR_TEST_HOOKS
  if (testTransportHandler_) {
    if (!testTransportHandler_(message)) {
      Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Failed to send message (msgId: 0x%llX)", msgId);
      return WebSocketSendStatus::Rejected;
    }
    return WebSocketSendStatus::Sent;
  }
#endif

  auto result = webSocket_->send(message, true);

  if (!result.success) {
    Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Failed to send message (msgId: 0x%llX)", msgId);
    return WebSocketSendStatus::Rejected;
  }

  Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Sent message (msgId: 0x%llX, size: %llu bytes, total: %zu bytes)", msgId,
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

BOOL WebSocketClient::IsConnected() const { return connected_.load(std::memory_order_relaxed); }

VOID WebSocketClient::OnMessage(const ix::WebSocketMessagePtr& msg) {
  switch (msg->type) {
    case ix::WebSocketMessageType::Open:
      Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Connected to ServerDB");
      connected_.store(true);
      FlushPendingMessages();
      if (connectionCallback_) {
        connectionCallback_(TRUE);
      }
      break;

    case ix::WebSocketMessageType::Close:
      Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Disconnected from ServerDB (code: %d, reason redacted: %zu bytes)",
          msg->closeInfo.code, msg->closeInfo.reason.size());
      connected_.store(false);
      break;

    case ix::WebSocketMessageType::Error:
      Log(EchoVR::LogLevel::Error, "[WEBSOCKET] Connection error (reason redacted: %zu bytes)",
          msg->errorInfo.reason.size());
      connected_.store(false);
      break;

    case ix::WebSocketMessageType::Message:
      if (msg->binary) {
        const std::string& payload = msg->str;
        size_t queueCapacity = 0;
        EnterCriticalSection(&receivedMessagesMutex_);
        if (receivedMessages_.size() < 1024) queueCapacity = 1024 - receivedMessages_.size();
        LeaveCriticalSection(&receivedMessagesMutex_);

        auto parsed = GameServer::ParseServerDbFrame(payload, queueCapacity);
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
          case GameServer::WebSocketFrameStatus::Complete:
            break;
          case GameServer::WebSocketFrameStatus::TooShort:
            Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Received malformed binary message (too short: %zu bytes)",
                payload.size());
            break;
          case GameServer::WebSocketFrameStatus::InvalidMagic:
            Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Received binary frame with invalid magic at offset %zu",
                parsed.errorOffset);
            break;
          case GameServer::WebSocketFrameStatus::TruncatedHeader:
            Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Truncated message header at offset %zu (%zu bytes remain)",
                parsed.errorOffset, parsed.remainingLength);
            break;
          case GameServer::WebSocketFrameStatus::TruncatedPayload:
            Log(EchoVR::LogLevel::Warning,
                "[WEBSOCKET] Message length exceeds frame (msgId: 0x%llX, length: %llu, remaining: %zu)",
                parsed.errorMessageId, static_cast<unsigned long long>(parsed.declaredLength),
                parsed.remainingLength);
            break;
          case GameServer::WebSocketFrameStatus::OversizedMessage:
            Log(EchoVR::LogLevel::Warning,
                "[WEBSOCKET] Dropped oversized message (msgId: 0x%llX, size: %llu bytes)",
                parsed.errorMessageId, static_cast<unsigned long long>(parsed.declaredLength));
            break;
          case GameServer::WebSocketFrameStatus::QueueLimit:
            queueFull = true;
            break;
        }
        if (queueFull) {
          Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Receive queue full (1024) — remaining frame messages dropped");
        }
        if (parsed.messages.size() > 1) {
          Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Parsed %zu messages from single frame (%zu bytes)",
              parsed.messages.size(), payload.size());
        }
      } else {
        Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Received unexpected text message: %s", msg->str.c_str());
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

  Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Flushing %zu pending messages", toSend.size());

  for (const auto& message : toSend) {
    Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Sending pending message: size=%zu bytes", message.size());
    auto result = webSocket_->send(message, true);
    if (!result.success) {
      Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Failed to send pending message");
    } else {
      Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Successfully sent pending message");
    }
  }
}

VOID WebSocketClient::ProcessReceivedMessages() {
  std::vector<GameServer::ReceivedWebSocketMessage> messagesToProcess;

  EnterCriticalSection(&receivedMessagesMutex_);
  messagesToProcess.swap(receivedMessages_);
  LeaveCriticalSection(&receivedMessagesMutex_);

  for (const auto& msg : messagesToProcess) {
    if (messageCallback_) {
      const VOID* data = msg.payload.empty() ? nullptr : msg.payload.data();
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
#endif
