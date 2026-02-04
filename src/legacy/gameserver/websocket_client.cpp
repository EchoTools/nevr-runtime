#include "websocket_client.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "common/echovr.h"

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

WebSocketClient::WebSocketClient() : webSocket_(std::make_unique<ix::WebSocket>()), connected_(FALSE) {
  // Initialize network system (required on Windows)
  // Note: Using static variable for one-time initialization across all instances
  static bool netSystemInitialized_ = false;
  if (!netSystemInitialized_) {
    ix::initNetSystem();
    netSystemInitialized_ = true;
  }

  // Set up the message callback
  webSocket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) { OnMessage(msg); });
}

WebSocketClient::~WebSocketClient() { Disconnect(); }

BOOL WebSocketClient::Connect(const CHAR* uri) {
  if (!uri || strlen(uri) == 0) {
    Log(EchoVR::LogLevel::Error, "[WEBSOCKET] Invalid URI provided for connection");
    return FALSE;
  }

  Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Connecting to ServerDB at %s", uri);

  // Set the URL
  webSocket_->setUrl(std::string(uri));

  // Start the connection (non-blocking)
  webSocket_->start();

  return TRUE;
}

VOID WebSocketClient::Disconnect() {
  if (webSocket_) {
    Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Disconnecting from ServerDB");
    webSocket_->stop();
    connected_ = FALSE;
  }
}

BOOL WebSocketClient::Send(EchoVR::SymbolId msgId, const VOID* data, UINT64 size) {
  std::vector<uint8_t> messageBuffer(sizeof(EchoVR::SymbolId) + sizeof(UINT64) + size);

  memcpy(messageBuffer.data(), &msgId, sizeof(EchoVR::SymbolId));
  memcpy(messageBuffer.data() + sizeof(EchoVR::SymbolId), &size, sizeof(UINT64));

  if (size > 0 && data != nullptr) {
    memcpy(messageBuffer.data() + sizeof(EchoVR::SymbolId) + sizeof(UINT64), data, size);
  }

  std::string message(messageBuffer.begin(), messageBuffer.end());

  if (!connected_) {
    pendingMessages_.push_back(message);
    Log(EchoVR::LogLevel::Debug,
        "[WEBSOCKET] Queued message (msgId: 0x%llX, size: %llu bytes, payload: %llu bytes) - will send when connected",
        msgId, size, size);
    return TRUE;
  }

  auto result = webSocket_->send(message, true);

  if (!result.success) {
    Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Failed to send message (msgId: 0x%llX)", msgId);
    return FALSE;
  }

  Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Sent message (msgId: 0x%llX, size: %llu bytes, total: %zu bytes)", msgId,
      size, messageBuffer.size());

  return TRUE;
}

VOID WebSocketClient::SetMessageHandler(MessageCallback callback) { messageCallback_ = callback; }

BOOL WebSocketClient::IsConnected() const { return connected_; }

VOID WebSocketClient::OnMessage(const ix::WebSocketMessagePtr& msg) {
  switch (msg->type) {
    case ix::WebSocketMessageType::Open:
      Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Connected to ServerDB");
      connected_ = TRUE;
      FlushPendingMessages();
      break;

    case ix::WebSocketMessageType::Close:
      Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Disconnected from ServerDB (code: %d, reason: %s)", msg->closeInfo.code,
          msg->closeInfo.reason.c_str());
      connected_ = FALSE;
      break;

    case ix::WebSocketMessageType::Error:
      Log(EchoVR::LogLevel::Error, "[WEBSOCKET] Connection error: %s", msg->errorInfo.reason.c_str());
      connected_ = FALSE;
      break;

    case ix::WebSocketMessageType::Message:
      if (msg->binary) {
        const std::string& payload = msg->str;
        const UINT64 MAGIC = 0xBB8CE7A278BB40F6;

        if (payload.size() >= sizeof(UINT64) + sizeof(EchoVR::SymbolId) + sizeof(UINT64)) {
          UINT64 magic;
          memcpy(&magic, payload.data(), sizeof(UINT64));

          if (magic != MAGIC) {
            Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Received message with invalid magic: 0x%llX (expected 0x%llX)",
                magic, MAGIC);
            break;
          }

          EchoVR::SymbolId msgId;
          memcpy(&msgId, payload.data() + sizeof(UINT64), sizeof(EchoVR::SymbolId));

          UINT64 length;
          memcpy(&length, payload.data() + sizeof(UINT64) + sizeof(EchoVR::SymbolId), sizeof(UINT64));

          const VOID* data = nullptr;
          UINT64 actualPayloadSize = payload.size() - sizeof(UINT64) - sizeof(EchoVR::SymbolId) - sizeof(UINT64);
          if (actualPayloadSize > 0) {
            data = payload.data() + sizeof(UINT64) + sizeof(EchoVR::SymbolId) + sizeof(UINT64);
          }

          Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Received message (msgId: 0x%llX, length: %llu, payload: %llu bytes)",
              msgId, length, actualPayloadSize);

          if (messageCallback_) {
            messageCallback_(msgId, data, actualPayloadSize);
          }
        } else {
          Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Received malformed binary message (too short: %zu bytes)",
              payload.size());
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
  if (pendingMessages_.empty()) {
    return;
  }

  Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Flushing %zu pending messages", pendingMessages_.size());

  for (const auto& message : pendingMessages_) {
    Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Sending pending message: size=%zu bytes", message.size());
    auto result = webSocket_->send(message, true);
    if (!result.success) {
      Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Failed to send pending message");
    } else {
      Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Successfully sent pending message");
    }
  }

  pendingMessages_.clear();
}
