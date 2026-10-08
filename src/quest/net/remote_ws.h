#pragma once
// The service-facing half of the Quest transport: SessionRouter::RemoteTransport over an abstract,
// blocking-free WebSocket client (WsConnector). Platform neutral: the libcurl implementation lives in
// curl_ws_connector.{h,cpp}; tests supply a fake.
//
// Transport policy, enforced HERE so no connector can weaken it:
//   * the remote URL must be wss://. A ws:// URL, an http(s):// URL or anything else is refused before the
//     connector is touched, and the refusal ends the session. There is no insecure mode and no option to
//     add one.
//   * one connect attempt per remote session. A failed attempt (TLS verification failure included) is
//     reported to the router as a remote error. The transport never retries, never downgrades the scheme
//     and never retries with verification relaxed; the game's own reconnect makes the next attempt.
//   * nothing sensitive is logged: not the URL (its query can carry credentials), not a header value.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/compat/session_router.h"

namespace quest_net {

enum class ConnectStatus {
  Ok,
  TlsVerificationFailed,  // the certificate chain or host name did not verify
  TlsError,               // the TLS handshake failed for another reason
  HandshakeRejected,      // TLS fine, the WebSocket upgrade was refused (http status in the result)
  NetworkError,           // DNS, refused, timed out, reset
  PolicyRefused,          // the connector itself refused the request (scheme, options)
};
const char* ConnectStatusName(ConnectStatus status);

enum class RecvStatus {
  Frame,   // `data` holds one complete message
  Idle,    // nothing yet (timeout, or Wake() was called)
  Closed,  // the peer sent a close frame
  Error,   // the connection broke
};

struct RecvResult {
  RecvStatus status = RecvStatus::Idle;
  std::string data;
  bool binary = true;
  uint16_t closeCode = 0;
};

class WsConnection {
 public:
  virtual ~WsConnection() = default;
  // Hands a whole message to the TLS layer. False when the connection is broken.
  virtual bool Send(std::string_view data, bool binary) = 0;
  // Waits up to timeoutMs for a message. Returns Idle early when Wake() is called.
  virtual RecvResult Recv(int timeoutMs) = 0;
  // Thread safe: interrupts a concurrent Recv.
  virtual void Wake() = 0;
  // Best-effort close frame; never blocks for the peer's reply.
  virtual void SendClose(uint16_t code) = 0;
};

struct Header {
  std::string name;
  std::string value;  // may be a credential: never logged
};

struct ConnectRequest {
  std::string url;  // wss://... ; may carry credentials in its query: never logged
  std::vector<Header> headers;
};

struct ConnectResult {
  ConnectStatus status = ConnectStatus::NetworkError;
  int httpStatus = 0;    // the upgrade response status when there was one
  int nativeCode = 0;    // the library's own code (CURLcode), for the log line
  std::unique_ptr<WsConnection> connection;  // set only when status is Ok
};

class WsConnector {
 public:
  virtual ~WsConnector() = default;
  // Blocks until connected or failed. Called from the remote's own worker thread.
  virtual ConnectResult Connect(const ConnectRequest& request) = 0;
};

// True for a URL the transport may dial: scheme exactly wss (any case), a non-empty host, no whitespace or
// control characters.
bool IsAcceptableRemoteUrl(const std::string& url);

// Builds the URL and headers for one remote session (auth route, matchmaker query changes). nullopt means
// the identity needed for the route is missing; the session ends rather than connecting unauthenticated.
using RequestBuilder = std::function<std::optional<ConnectRequest>(const SessionRouter::RemoteOpenRequest&)>;

class ConnectorRemoteTransport final : public SessionRouter::RemoteTransport {
 public:
  struct Config {
    std::size_t maxQueuedBytes = 4u * 1024u * 1024u;  // frames waiting for the worker to write
    int pollMs = 200;
    SessionRouter::LogSink log;
  };

  ConnectorRemoteTransport(WsConnector* connector, RequestBuilder builder, Config config);
  ~ConnectorRemoteTransport() override;
  ConnectorRemoteTransport(const ConnectorRemoteTransport&) = delete;
  ConnectorRemoteTransport& operator=(const ConnectorRemoteTransport&) = delete;

  void Attach(SessionRouter::Router* router) { router_ = router; }
  // Ends every session (close frame, no router callbacks) and joins the workers.
  void Stop();

  bool Open(const SessionRouter::RemoteOpenRequest& request) override;
  SessionRouter::SendResult Send(SessionRouter::RemoteId remote, std::string_view frame, bool binary) override;
  void Close(SessionRouter::RemoteId remote, uint16_t code) override;

 private:
  struct State;
  void Worker(std::shared_ptr<State> state, ConnectRequest request);
  std::shared_ptr<State> Find(SessionRouter::RemoteId remote);
  void Reap();
  void Log(SessionRouter::LogLevel level, const std::string& line);

  WsConnector* connector_;
  RequestBuilder builder_;
  Config config_;
  SessionRouter::Router* router_ = nullptr;
  std::mutex mutex_;  // guards states_ only
  std::map<SessionRouter::RemoteId, std::shared_ptr<State>> states_;
  std::atomic<bool> stopped_{false};
};

}  // namespace quest_net
