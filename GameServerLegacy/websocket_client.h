#pragma once

#include <functional>
#include <memory>
#include <string>

#include "common/echovr.h"
#include "common/pch.h"

// Forward declare ixwebsocket types to avoid including the full header
namespace ix {
class WebSocket;
struct WebSocketMessagePtr;
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
  /// Checks if the WebSocket is currently connected.
  /// </summary>
  /// <returns>TRUE if connected, FALSE otherwise</returns>
  BOOL IsConnected() const;

 private:
  // ixwebsocket instance (using unique_ptr for forward declaration)
  std::unique_ptr<ix::WebSocket> webSocket_;

  // Message receive callback
  MessageCallback messageCallback_;

  // Connection state
  BOOL connected_;

  // Internal message handler for ixwebsocket
  VOID OnMessage(const ix::WebSocketMessagePtr& msg);
};
