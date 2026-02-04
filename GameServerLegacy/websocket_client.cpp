#include "websocket_client.h"

#include <cstring>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include "common/echovr.h"
#include "common/pch.h"

// Logging wrapper
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

WebSocketClient::~WebSocketClient() {
  Disconnect();
}

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
  if (!connected_) {
    Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Cannot send message - not connected to ServerDB");
    return FALSE;
  }

  // Build the message: [8 bytes: SymbolId][N bytes: payload]
  // Use vector for binary data for better performance and semantics
  std::vector<uint8_t> messageBuffer(sizeof(EchoVR::SymbolId) + size);

  // Copy the SymbolId (first 8 bytes)
  memcpy(messageBuffer.data(), &msgId, sizeof(EchoVR::SymbolId));

  // Copy the payload (remaining bytes)
  if (size > 0 && data != nullptr) {
    memcpy(messageBuffer.data() + sizeof(EchoVR::SymbolId), data, size);
  }

  // Send as binary message
  // Note: ixwebsocket API requires std::string for send(). The conversion creates a copy,
  // but this is unavoidable with the current library API (no zero-copy send available).
  std::string message(messageBuffer.begin(), messageBuffer.end());
  auto result = webSocket_->send(message, true);  // true = send as binary

  if (!result.success) {
    Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Failed to send message: %s", result.errorStr.c_str());
    return FALSE;
  }

  Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Sent message (msgId: 0x%llX, size: %llu bytes)", msgId,
      size + sizeof(EchoVR::SymbolId));

  return TRUE;
}

VOID WebSocketClient::SetMessageHandler(MessageCallback callback) {
  messageCallback_ = callback;
}

BOOL WebSocketClient::IsConnected() const {
  return connected_;
}

VOID WebSocketClient::OnMessage(const ix::WebSocketMessagePtr& msg) {
  switch (msg->type) {
    case ix::WebSocketMessageType::Open:
      Log(EchoVR::LogLevel::Info, "[WEBSOCKET] Connected to ServerDB");
      connected_ = TRUE;
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
      // Handle incoming message
      if (msg->binary) {
        // Binary message received - parse it
        const std::string& payload = msg->str;

        // Ensure we have at least a SymbolId (8 bytes)
        if (payload.size() >= sizeof(EchoVR::SymbolId)) {
          // Extract the SymbolId (first 8 bytes)
          EchoVR::SymbolId msgId;
          memcpy(&msgId, payload.data(), sizeof(EchoVR::SymbolId));

          // Extract the payload (remaining bytes)
          const VOID* data = nullptr;
          UINT64 size = payload.size() - sizeof(EchoVR::SymbolId);
          if (size > 0) {
            data = payload.data() + sizeof(EchoVR::SymbolId);
          }
          // Note: data may be nullptr when size is 0, which is valid for empty payloads

          Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Received message (msgId: 0x%llX, size: %llu bytes)", msgId,
              payload.size());

          // Invoke the callback if set
          if (messageCallback_) {
            messageCallback_(msgId, data, size);
          }
        } else {
          Log(EchoVR::LogLevel::Warning, "[WEBSOCKET] Received malformed binary message (too short)");
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
      // Message fragments - handled automatically by ixwebsocket
      break;
  }
}
