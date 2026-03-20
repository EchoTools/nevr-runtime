#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include "echovr.h"

namespace ix {
class WebSocket;
struct WebSocketMessage;
using WebSocketMessagePtr = std::unique_ptr<WebSocketMessage>;
}  // namespace ix

/// <summary>
/// WebSocket client for ServerDB communication.
/// Handles connection lifecycle and binary message exchange with ServerDB.
/// Thread-safe and non-blocking - suitable for use in game Update() loop.
/// </summary>
class WebSocketClient {
 public:
  /// <summary>
  /// Callback function type for receiving messages from ServerDB.
  /// Parameters: msgId (SymbolId), payload data pointer, payload size
  /// </summary>
  using MessageCallback = std::function<VOID(EchoVR::SymbolId msgId, const VOID* data, UINT64 size)>;

  WebSocketClient();
  ~WebSocketClient();

  /// <summary>
  /// Connects to the ServerDB WebSocket service.
  /// Non-blocking - connection happens asynchronously.
  /// </summary>
  /// <param name="uri">WebSocket URI (e.g., "ws://localhost:777/serverdb")</param>
  /// <returns>TRUE if connection initiated successfully, FALSE on error</returns>
  BOOL Connect(const CHAR* uri);

  /// <summary>
  /// Disconnects from the ServerDB WebSocket service.
  /// Gracefully closes the connection.
  /// </summary>
  VOID Disconnect();

  /// <summary>
  /// Sends a binary message to ServerDB.
  /// Message format: [8 bytes: SymbolId][N bytes: payload]
  /// Non-blocking - message is queued for sending.
  /// </summary>
  /// <param name="msgId">The 64-bit symbol identifying the message type</param>
  /// <param name="data">Pointer to the message payload</param>
  /// <param name="size">Size of the payload in bytes</param>
  /// <returns>TRUE if message queued successfully, FALSE on error</returns>
  BOOL Send(EchoVR::SymbolId msgId, const VOID* data, UINT64 size);

  /// <summary>
  /// Sets the callback function to be invoked when a message is received.
  /// </summary>
  /// <param name="callback">The callback function</param>
  VOID SetMessageHandler(MessageCallback callback);

  /// <summary>
  /// Callback function type for connection state changes.
  /// Parameter: TRUE if connected, FALSE if disconnected
  /// </summary>
  using ConnectionCallback = std::function<VOID(BOOL connected)>;

  /// <summary>
  /// Sets the callback function to be invoked when connection state changes.
  /// Called on reconnection after a disconnect, enabling re-registration.
  /// </summary>
  VOID SetConnectionHandler(ConnectionCallback callback);

  /// <summary>
  /// Checks if the WebSocket is currently connected.
  /// </summary>
  /// <returns>TRUE if connected, FALSE otherwise</returns>
  BOOL IsConnected() const;

 private:
  // ixwebsocket instance (using unique_ptr for forward declaration)
  std::unique_ptr<ix::WebSocket> webSocket_;

  // Message receive callback
  MessageCallback messageCallback_;

  // Connection state change callback
  ConnectionCallback connectionCallback_;

  // Connection state
  BOOL connected_;

  // Message queue for messages sent before connection established
  std::vector<std::string> pendingMessages_;

  // Buffer to store last received message payload (keeps pointer valid during callback)
  std::vector<UINT8> lastReceivedPayload_;

  // Message queue for processing on main thread (thread-safe)
  struct ReceivedMessage {
    EchoVR::SymbolId msgId;
    std::vector<UINT8> payload;
    UINT64 timestamp;  // For deduplication
  };
  std::vector<ReceivedMessage> receivedMessages_;
  CRITICAL_SECTION receivedMessagesMutex_;

  // Last processed message for deduplication (msgId + first 8 bytes of payload)
  EchoVR::SymbolId lastMsgId_;
  UINT64 lastPayloadHash_;
  UINT64 lastMsgTimestamp_;

  // Internal message handler for ixwebsocket
  VOID OnMessage(const ix::WebSocketMessagePtr& msg);

  // Flush pending messages after connection is established
  VOID FlushPendingMessages();

 public:
  // Process queued received messages (call from main thread)
  VOID ProcessReceivedMessages();
};
