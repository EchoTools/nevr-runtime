#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "abi/echovr.h"
#include "runtime/server/websocket_frame.h"

namespace ix {
class WebSocket;
struct WebSocketMessage;
using WebSocketMessagePtr = std::unique_ptr<WebSocketMessage>;
}  // namespace ix

enum class WebSocketSendStatus {
  Rejected,
  Queued,
  Sent,
};

/// <summary>
/// WebSocket client for ServerDB communication.
/// Handles connection lifecycle and binary message exchange with ServerDB.
/// Thread-safe and non-blocking - suitable for use in game Update() loop.
/// </summary>
class WebSocketClient {
 public:
  /// <summary>
  /// Callback function type for receiving messages from ServerDB.
  /// Parameters: msgId (SymbolId), payload data pointer, payload size.
  /// The payload is a copy owned by ProcessReceivedMessages(), valid only for the
  /// duration of the call (nullptr when size is 0). It is deliberately writable:
  /// handlers forward it to CBroadcaster::ReceiveLocalEvent, whose listeners take
  /// it as mutable (issue #43).
  /// </summary>
  using MessageCallback = std::function<VOID(EchoVR::SymbolId msgId, VOID* data, UINT64 size)>;

  WebSocketClient();
  ~WebSocketClient();

  /// <summary>
  /// Connects to the ServerDB WebSocket service.
  /// Non-blocking - connection happens asynchronously.
  /// </summary>
  /// <param name="uri">WebSocket URI (e.g., "ws://localhost:777/serverdb")</param>
  /// <returns>TRUE if connection initiated successfully, FALSE on error</returns>
  BOOL Connect(const CHAR* uri, const std::string& bearerToken = "");

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
  WebSocketSendStatus SendWithStatus(EchoVR::SymbolId msgId, const VOID* data, UINT64 size);

  // Drop messages queued while disconnected. End-of-session calls this after
  // attempting CODE_ENDED so that a queued event cannot leak into a later registration.
  size_t DiscardPendingMessages();

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

  /// Mints a fresh bearer token; returns "" when none could be acquired. It runs
  /// on ixwebsocket's thread and may block on HTTP.
  using BearerTokenRefresher = std::function<std::string()>;

  /// Issue #39. The bearer token given to Connect is stored as an extra header,
  /// and ixwebsocket's automatic reconnect re-presents that stored header. When
  /// ServerDB rejects a reconnect with HTTP 401 (an expired or revoked JWT), the
  /// client calls this refresher and the next attempt presents the new token.
  VOID SetBearerTokenRefresher(BearerTokenRefresher refresher);

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

  // Connection state (written from ixwebsocket callback thread, read from main thread)
  std::atomic<bool> connected_{false};

  // Bearer auth (#39). Connect writes on the game thread; the 401 path reads
  // and writes on ixwebsocket's thread.
  std::mutex bearerTokenMutex_;
  std::string bearerToken_;
  BearerTokenRefresher bearerTokenRefresher_;
  std::atomic<uint32_t> bearerTokenRefreshCount_{0};

  // Message queue for messages sent before connection established
  std::vector<std::string> pendingMessages_;

  // Message queue for processing on main thread (thread-safe)
  std::vector<nevr_game_server::ReceivedWebSocketMessage> receivedMessages_;
  CRITICAL_SECTION receivedMessagesMutex_;
#ifdef NEVR_TEST_HOOKS
  std::function<bool(const std::string&)> testTransportHandler_;
#endif

  // Internal message handler for ixwebsocket
  VOID OnMessage(const ix::WebSocketMessagePtr& msg);

  // Flush pending messages after connection is established
  VOID FlushPendingMessages();

  // Stores the token and hands ixwebsocket the Authorization header.
  VOID ApplyBearerToken(const std::string& token);

  // ServerDB answered the upgrade with 401: mint a new token for the next attempt.
  VOID RefreshBearerTokenAfterRejection();

 public:
  // Process queued received messages (call from main thread)
  VOID ProcessReceivedMessages();

  /// Disables automatic reconnection so the next disconnect is final.
  VOID DisableReconnection();

#ifdef NEVR_TEST_HOOKS
  void TestSetConnected(bool connected);
  std::vector<std::string> TestCopyPendingMessages();
  void TestSetTransportHandler(std::function<bool(const std::string&)> handler);
  // Feeds ProcessReceivedMessages() without an ixwebsocket connection.
  void TestEnqueueReceivedMessage(nevr_game_server::ReceivedWebSocketMessage message);
#endif
};
