#include "quest/net/remote_ws.h"

#include <cctype>
#include <cstdio>

namespace quest_net {

using nevr_session_router::LogLevel;
using nevr_session_router::RemoteId;
using nevr_session_router::SendResult;

const char* ConnectStatusName(ConnectStatus status) {
  switch (status) {
    case ConnectStatus::Ok: return "ok";
    case ConnectStatus::TlsVerificationFailed: return "tls verification failed";
    case ConnectStatus::TlsError: return "tls handshake failed";
    case ConnectStatus::HandshakeRejected: return "websocket upgrade rejected";
    case ConnectStatus::NetworkError: return "network error";
    case ConnectStatus::PolicyRefused: return "refused by transport policy";
  }
  return "unknown";
}

bool IsAcceptableRemoteUrl(const std::string& url) {
  static const char kScheme[] = "wss://";
  const std::size_t n = sizeof(kScheme) - 1;
  if (url.size() <= n) return false;
  for (std::size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(url[i])) != kScheme[i]) return false;
  }
  for (const char c : url) {
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return false;
  }
  const char first = url[n];
  if (first == '/' || first == '?' || first == '#' || first == ':') return false;
  // No userinfo: credentials in the authority would end up in logs and proxies; the auth route uses headers.
  const std::size_t authorityEnd = url.find_first_of("/?#", n);
  const std::string authority = url.substr(n, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - n);
  return authority.find('@') == std::string::npos;
}

struct ConnectorRemoteTransport::State {
  RemoteId id = 0;
  std::thread thread;
  std::atomic<bool> done{false};

  std::mutex mutex;  // guards everything below
  std::deque<std::pair<std::string, bool>> outbound;
  std::size_t queuedBytes = 0;
  bool blocked = false;
  bool closeRequested = false;
  uint16_t closeCode = 1000;
  std::shared_ptr<WsConnection> connection;  // shared so Wake() from another thread cannot outlive it
};

ConnectorRemoteTransport::ConnectorRemoteTransport(WsConnector* connector, RequestBuilder builder, Config config)
    : connector_(connector), builder_(std::move(builder)), config_(std::move(config)) {}

ConnectorRemoteTransport::~ConnectorRemoteTransport() { Stop(); }

void ConnectorRemoteTransport::Log(LogLevel level, const std::string& line) {
  if (config_.log) config_.log(level, line);
}

std::shared_ptr<ConnectorRemoteTransport::State> ConnectorRemoteTransport::Find(RemoteId remote) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = states_.find(remote);
  return it == states_.end() ? nullptr : it->second;
}

void ConnectorRemoteTransport::Reap() {
  std::vector<std::shared_ptr<State>> finished;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = states_.begin(); it != states_.end();) {
      if (it->second->done) {
        finished.push_back(it->second);
        it = states_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& state : finished) {
    if (state->thread.joinable()) state->thread.join();
  }
}

bool ConnectorRemoteTransport::Open(const nevr_session_router::RemoteOpenRequest& request) {
  if (router_ == nullptr || stopped_) return false;
  Reap();
  std::optional<ConnectRequest> built = builder_ ? builder_(request) : std::nullopt;
  if (!built.has_value()) {
    Log(LogLevel::Error, "[remote] remote=" + std::to_string(request.remote) +
                             " not started: no connect request (identity or configuration missing)");
    return false;
  }
  if (!IsAcceptableRemoteUrl(built->url)) {
    // Never log the URL: its query may carry credentials. The scheme is the only thing worth naming.
    Log(LogLevel::Error, "[remote] remote=" + std::to_string(request.remote) +
                             " refused: the remote URL must be wss:// with a host (plaintext is never used)");
    return false;
  }
  auto state = std::make_shared<State>();
  state->id = request.remote;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    states_[request.remote] = state;
  }
  ConnectRequest connectRequest = std::move(*built);
  state->thread = std::thread([this, state, connectRequest]() mutable { Worker(state, std::move(connectRequest)); });
  return true;
}

void ConnectorRemoteTransport::Worker(std::shared_ptr<State> state, ConnectRequest request) {
  const RemoteId id = state->id;
  ConnectResult result = connector_->Connect(request);  // exactly one attempt
  request = ConnectRequest();                            // drop the URL and credentials now
  if (result.status != ConnectStatus::Ok || !result.connection) {
    const ConnectStatus status = result.status == ConnectStatus::Ok ? ConnectStatus::NetworkError : result.status;
    Log(LogLevel::Warning, "[remote] remote=" + std::to_string(id) + " connect failed: " + ConnectStatusName(status) +
                               " http_status=" + std::to_string(result.httpStatus) +
                               " native_code=" + std::to_string(result.nativeCode) + " (no retry, no downgrade)");
    state->done = true;
    router_->OnRemoteError(id, result.httpStatus, ConnectStatusName(status));
    return;
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->connection = std::shared_ptr<WsConnection>(std::move(result.connection));
  }
  const std::shared_ptr<WsConnection> conn = state->connection;
  Log(LogLevel::Info, "[remote] remote=" + std::to_string(id) + " connected");
  router_->OnRemoteOpen(id);

  const char* ended = nullptr;     // set when the session ends from our side of the wire
  bool reportClose = false;
  uint16_t peerCode = 0;
  while (ended == nullptr && !reportClose) {
    std::deque<std::pair<std::string, bool>> batch;
    bool close = false;
    uint16_t closeCode = 1000;
    bool owe = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      batch.swap(state->outbound);
      state->queuedBytes = 0;
      close = state->closeRequested;
      closeCode = state->closeCode;
      if (state->blocked) {
        state->blocked = false;
        owe = true;
      }
    }
    for (auto& item : batch) {
      if (!conn->Send(item.first, item.second)) {
        ended = "send failed";
        break;
      }
    }
    if (ended != nullptr) break;
    if (owe) router_->OnRemoteWritable(id);
    if (close || stopped_) {
      conn->SendClose(close ? closeCode : static_cast<uint16_t>(1001));
      state->done = true;
      return;  // the router (or Stop) asked for this: nothing to report back
    }
    RecvResult received = conn->Recv(config_.pollMs);
    switch (received.status) {
      case RecvStatus::Frame:
        router_->OnRemoteFrame(id, std::move(received.data), received.binary);
        break;
      case RecvStatus::Idle:
        break;
      case RecvStatus::Closed:
        reportClose = true;
        peerCode = received.closeCode;
        break;
      case RecvStatus::Error:
        ended = "connection error";
        break;
    }
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->connection.reset();
  }
  state->done = true;
  if (reportClose) {
    Log(LogLevel::Info, "[remote] remote=" + std::to_string(id) + " peer closed code=" + std::to_string(peerCode));
    router_->OnRemoteClose(id, peerCode);
  } else if (ended != nullptr) {
    Log(LogLevel::Warning, "[remote] remote=" + std::to_string(id) + " " + ended);
    router_->OnRemoteError(id, 0, ended);
  }
}

SendResult ConnectorRemoteTransport::Send(RemoteId remote, std::string_view frame, bool binary) {
  const std::shared_ptr<State> state = Find(remote);
  if (!state) return SendResult::Failed;
  std::shared_ptr<WsConnection> conn;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closeRequested || state->done) return SendResult::Failed;
    if (state->queuedBytes + frame.size() > config_.maxQueuedBytes) {
      state->blocked = true;
      conn = state->connection;
    } else {
      state->outbound.emplace_back(std::string(frame), binary);
      state->queuedBytes += frame.size();
      conn = state->connection;
      if (conn) conn->Wake();
      return SendResult::Sent;
    }
  }
  if (conn) conn->Wake();
  return SendResult::WouldBlock;
}

void ConnectorRemoteTransport::Close(RemoteId remote, uint16_t code) {
  const std::shared_ptr<State> state = Find(remote);
  if (!state) return;
  std::shared_ptr<WsConnection> conn;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closeRequested = true;
    state->closeCode = code;
    conn = state->connection;
  }
  if (conn) conn->Wake();
}

void ConnectorRemoteTransport::Stop() {
  stopped_ = true;
  std::vector<std::shared_ptr<State>> all;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : states_) all.push_back(entry.second);
    states_.clear();
  }
  for (auto& state : all) {
    std::shared_ptr<WsConnection> conn;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->closeRequested = true;
      conn = state->connection;
    }
    if (conn) conn->Wake();
  }
  for (auto& state : all) {
    if (state->thread.joinable()) state->thread.join();
  }
}

}  // namespace quest_net
