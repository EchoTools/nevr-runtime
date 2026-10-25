#include "runtime/compat/ws_session_transport.h"

#include <thread>
#include <utility>

namespace nevr_ws_transport {

using nevr_session_router::SendResult;

GameSockets::GameSockets(GameSocketLookup lookup) : lookup_(std::move(lookup)) {}

GameId GameSockets::Register(const ix::WebSocket* socket, int connIdx) {
  std::lock_guard<std::mutex> lock(mutex_);
  const GameId id = next_++;
  byId_[id] = {socket, connIdx};
  bySocket_[socket] = id;
  return id;
}

std::optional<GameId> GameSockets::IdOf(const ix::WebSocket* socket) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = bySocket_.find(socket);
  if (it == bySocket_.end()) return std::nullopt;
  return it->second;
}

int GameSockets::ConnIdxOf(const ix::WebSocket* socket) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = bySocket_.find(socket);
  if (it == bySocket_.end()) return -1;
  const auto entry = byId_.find(it->second);
  return entry == byId_.end() ? -1 : entry->second.connIdx;
}

void GameSockets::Forget(const ix::WebSocket* socket) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = bySocket_.find(socket);
  if (it == bySocket_.end()) return;
  byId_.erase(it->second);
  bySocket_.erase(it);
}

SendResult GameSockets::Send(GameId game, std::string_view frame, bool binary) {
  const ix::WebSocket* raw = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byId_.find(game);
    if (it == byId_.end()) return SendResult::Failed;
    raw = it->second.socket;
  }
  const std::shared_ptr<ix::WebSocket> socket = lookup_(raw);  // keeps it alive for the send
  if (!socket) return SendResult::Failed;
  std::string data(frame);
  if (rewrite_) {
    if (std::optional<std::string> replacement = rewrite_(data, binary)) data = std::move(*replacement);
  }
  const ix::WebSocketSendInfo info = binary ? socket->sendBinary(data) : socket->sendText(data);
  return info.success ? SendResult::Sent : SendResult::Failed;
}

void GameSockets::Close(GameId game, uint16_t code, std::string_view reason) {
  const ix::WebSocket* raw = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byId_.find(game);
    if (it == byId_.end()) return;
    raw = it->second.socket;
  }
  const std::shared_ptr<ix::WebSocket> socket = lookup_(raw);
  if (!socket) return;
  // Not inline: close() waits on the socket's own thread, which may be inside the router waiting for the lock
  // this call was made under, or inside this very callback.
  std::thread([socket, code, text = std::string(reason)]() { socket->close(code, text); }).detach();
}

RemoteSockets::RemoteSockets(RemoteHooks hooks) : hooks_(std::move(hooks)) {}

bool RemoteSockets::Open(const nevr_session_router::RemoteOpenRequest& request) {
  std::vector<std::shared_ptr<ix::WebSocket>> destroy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || router_ == nullptr) return false;
    destroy.swap(retired_);  // destroyed below, after the lock: a retired socket's thread may still want it
  }
  destroy.clear();
  std::shared_ptr<ix::WebSocket> socket = hooks_.prepare ? hooks_.prepare(request) : nullptr;
  if (!socket) return false;
  const RemoteId id = request.remote;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return false;
    live_[id] = {socket, request};
  }
  socket->setOnMessageCallback(hooks_.guard([this, id](const ix::WebSocketMessagePtr& message) { OnEvent(id, message); }));
  socket->start();
  return true;
}

SendResult RemoteSockets::Send(RemoteId remote, std::string_view frame, bool binary) {
  std::shared_ptr<ix::WebSocket> socket;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = live_.find(remote);
    if (it == live_.end()) return SendResult::Failed;
    socket = it->second.socket;
  }
  const std::string data(frame);
  const ix::WebSocketSendInfo info = binary ? socket->sendBinary(data) : socket->sendText(data);
  return info.success ? SendResult::Sent : SendResult::Failed;
}

void RemoteSockets::Close(RemoteId remote, uint16_t code) {
  std::shared_ptr<ix::WebSocket> socket;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;  // StopAll owns every stop from here
    const auto it = live_.find(remote);
    if (it == live_.end()) return;
    socket = it->second.socket;
  }
  // stop() joins the socket's thread; the router may be calling from that thread (an oversized frame).
  std::thread([socket, code]() { socket->stop(code); }).detach();
}

std::size_t RemoteSockets::StopAll() {
  std::vector<std::shared_ptr<ix::WebSocket>> sockets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    for (auto& entry : live_) sockets.push_back(entry.second.socket);
    live_.clear();
    for (auto& socket : retired_) sockets.push_back(std::move(socket));
    retired_.clear();
  }
  for (const auto& socket : sockets) socket->stop();  // N60: outside every lock
  return sockets.size();
}

void RemoteSockets::Retire(RemoteId remote) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = live_.find(remote);
  if (it == live_.end()) return;
  retired_.push_back(std::move(it->second.socket));
  live_.erase(it);
}

void RemoteSockets::OnEvent(RemoteId remote, const ix::WebSocketMessagePtr& message) {
  nevr_session_router::RemoteOpenRequest request;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = live_.find(remote);
    if (it == live_.end()) return;  // retired or stopped: nothing to tell the router
    request = it->second.request;
  }
  switch (message->type) {
    case ix::WebSocketMessageType::Open:
      router_->OnRemoteOpen(remote);
      break;
    case ix::WebSocketMessageType::Message:
      if (hooks_.tap) hooks_.tap(request, message->str);
      router_->OnRemoteFrame(remote, message->str, message->binary);
      break;
    case ix::WebSocketMessageType::Close:
      if (hooks_.log) {
        hooks_.log(nevr_session_router::LogLevel::Info,
                   "remote closed remote=" + std::to_string(remote) + " code=" + std::to_string(message->closeInfo.code));
      }
      Retire(remote);
      router_->OnRemoteClose(remote, message->closeInfo.code);
      break;
    case ix::WebSocketMessageType::Error:
      if (hooks_.log) {
        hooks_.log(nevr_session_router::LogLevel::Warning,
                   "remote error remote=" + std::to_string(remote) +
                       " http_status=" + std::to_string(message->errorInfo.http_status) +
                       " retries=" + std::to_string(message->errorInfo.retries));
      }
      Retire(remote);
      router_->OnRemoteError(remote, message->errorInfo.http_status, "remote error");
      break;
    default:
      break;
  }
}

}  // namespace nevr_ws_transport
