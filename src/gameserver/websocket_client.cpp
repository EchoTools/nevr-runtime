#include "websocket_client.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "echovr.h"

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

WebSocketClient::WebSocketClient()
    : webSocket_(std::make_unique<ix::WebSocket>()),
      connected_(FALSE),
      lastMsgId_(0),
      lastPayloadHash_(0),
      lastMsgTimestamp_(0) {
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

          UINT64 actualPayloadSize = payload.size() - sizeof(UINT64) - sizeof(EchoVR::SymbolId) - sizeof(UINT64);

          Log(EchoVR::LogLevel::Info,
              "[WEBSOCKET] Received message (msgId: 0x%llX, length field: %llu, calculated payload: %llu bytes, total "
              "frame: %zu bytes)",
              msgId, length, actualPayloadSize, payload.size());

          // Deduplicate messages: check if this is the same as the last message within 100ms
          UINT64 payloadHash = 0;
          if (actualPayloadSize >= 8) {
            memcpy(&payloadHash, payload.data() + sizeof(UINT64) + sizeof(EchoVR::SymbolId) + sizeof(UINT64), 8);
          }
          UINT64 currentTimestamp = GetTickCount64();

          if (msgId == lastMsgId_ && payloadHash == lastPayloadHash_ && (currentTimestamp - lastMsgTimestamp_) < 100) {
            Log(EchoVR::LogLevel::Debug, "[WEBSOCKET] Dropping duplicate message (msgId: 0x%llX)", msgId);
            return;  // Exit OnMessage entirely, not just the switch case
          }

          lastMsgId_ = msgId;
          lastPayloadHash_ = payloadHash;
          lastMsgTimestamp_ = currentTimestamp;

          // Queue message for processing on main thread (thread-safe)
          ReceivedMessage receivedMsg;
          receivedMsg.msgId = msgId;
          receivedMsg.timestamp = currentTimestamp;
          if (actualPayloadSize > 0) {
            receivedMsg.payload.resize(actualPayloadSize);
            memcpy(receivedMsg.payload.data(),
                   payload.data() + sizeof(UINT64) + sizeof(EchoVR::SymbolId) + sizeof(UINT64), actualPayloadSize);
          }

          EnterCriticalSection(&receivedMessagesMutex_);
          receivedMessages_.push_back(std::move(receivedMsg));
          LeaveCriticalSection(&receivedMessagesMutex_);
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

VOID WebSocketClient::ProcessReceivedMessages() {
  std::vector<ReceivedMessage> messagesToProcess;

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
