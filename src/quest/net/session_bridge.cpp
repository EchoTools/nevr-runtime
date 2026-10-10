#include "quest/net/session_bridge.h"

#include <utility>

#include "runtime/compat/evr_codec.h"
#include "runtime/server/serverdb_uri.h"

namespace quest_net {

using SessionRouter::LogLevel;

namespace {

LoopbackGameServer::Config WithLog(LoopbackGameServer::Config c, const SessionRouter::LogSink& log) {
  if (!c.log) c.log = log;
  return c;
}

CurlWsConnector::Config WithLog(CurlWsConnector::Config c, const SessionRouter::LogSink& log) {
  if (!c.log) c.log = log;
  return c;
}

}  // namespace

// Server -> game direction: the router's Send toward the loopback server. A frame is shown to the tap only
// when the inner transport took it (SendResult::Sent), so a WouldBlock the router retries is not observed
// twice.
class SessionBridge::TappedGames final : public SessionRouter::GameTransport {
 public:
  TappedGames(SessionRouter::GameTransport* inner, FrameTap* tap) : inner_(inner), tap_(tap) {}

  SessionRouter::SendResult Send(SessionRouter::GameId game, std::string_view frame, bool binary) override {
    const SessionRouter::SendResult result = inner_->Send(game, frame, binary);
    if (result == SessionRouter::SendResult::Sent) tap_->ServerToGame(frame);
    return result;
  }
  void Close(SessionRouter::GameId game, std::uint16_t code, std::string_view reason) override {
    inner_->Close(game, code, reason);
  }
  void SetIdleExempt(SessionRouter::GameId game, bool exempt) override { inner_->SetIdleExempt(game, exempt); }

 private:
  SessionRouter::GameTransport* inner_;
  FrameTap* tap_;
};

// Game -> server direction (the router's Send toward the remote, which also carries the frames the router
// itself injects), and the side channel to the login session.
class SessionBridge::TappedRemotes final : public SessionRouter::RemoteTransport {
 public:
  TappedRemotes(SessionRouter::RemoteTransport* inner, FrameTap* tap) : inner_(inner), tap_(tap) {}

  bool Open(const SessionRouter::RemoteOpenRequest& request) override {
    if (request.role == SessionRouter::Role::Login && !request.standaloneMatchmaker) {
      // A new login session: nothing may be sent on it until the service accepts the login.
      loginAccepted_.store(false, std::memory_order_release);
      loginRemote_.store(request.remote, std::memory_order_release);
    }
    return inner_->Open(request);
  }
  SessionRouter::SendResult Send(SessionRouter::RemoteId remote, std::string_view frame, bool binary) override {
    const SessionRouter::SendResult result = inner_->Send(remote, frame, binary);
    if (result == SessionRouter::SendResult::Sent) tap_->GameToServer(frame);
    return result;
  }
  void Close(SessionRouter::RemoteId remote, std::uint16_t code) override {
    SessionRouter::RemoteId expected = remote;
    if (loginRemote_.compare_exchange_strong(expected, SessionRouter::kNoRemote, std::memory_order_acq_rel)) {
      loginAccepted_.store(false, std::memory_order_release);
    }
    inner_->Close(remote, code);
  }

  bool SendToLogin(std::string_view frame) {
    if (!loginAccepted_.load(std::memory_order_acquire)) return false;
    const SessionRouter::RemoteId remote = loginRemote_.load(std::memory_order_acquire);
    if (remote == SessionRouter::kNoRemote) return false;
    return inner_->Send(remote, frame, /*binary=*/true) == SessionRouter::SendResult::Sent;
  }
  // The router saw LoginSuccess on the login session: from here SendToLogin may send.
  void MarkLoginAccepted() { loginAccepted_.store(true, std::memory_order_release); }

 private:
  SessionRouter::RemoteTransport* inner_;
  FrameTap* tap_;
  std::atomic<SessionRouter::RemoteId> loginRemote_{SessionRouter::kNoRemote};
  std::atomic<bool> loginAccepted_{false};
};

SessionBridge::SessionBridge(Config config) : config_(std::move(config)), tap_(FrameTapSinks{}) {
  // The login is accepted when the service's LoginSuccess passes the tap: the side channel opens, and the
  // configured consumer (the social facade's local account) hears about it.
  FrameTapSinks sinks = config_.tap;
  std::function<void(std::uint64_t)> consumer = std::move(sinks.onLoginSuccess);
  sinks.onLoginSuccess = [this, consumer](std::uint64_t account) {
    if (remotesTap_) remotesTap_->MarkLoginAccepted();
    if (consumer) consumer(account);
  };
  tap_ = FrameTap(std::move(sinks));

  WsConnector* connector = config_.connector;
  if (connector == nullptr) {
    ownedConnector_ = std::make_unique<CurlWsConnector>(WithLog(config_.tls, config_.log));
    connector = ownedConnector_.get();
  }
  ConnectorRemoteTransport::Config remoteConfig;
  remoteConfig.log = config_.log;
  remotes_ = std::make_unique<ConnectorRemoteTransport>(
      connector, [this](const SessionRouter::RemoteOpenRequest& r) { return BuildRequest(r); }, remoteConfig);
  server_ = std::make_unique<LoopbackGameServer>(WithLog(config_.loopback, config_.log));
  gamesTap_ = std::make_unique<TappedGames>(server_.get(), &tap_);
  remotesTap_ = std::make_unique<TappedRemotes>(remotes_.get(), &tap_);

  SessionRouter::Options options;
  options.limits = config_.limits;
  // No buildLogin: the game sends its own login (see the header).
  options.loginGate = config_.loginGate;
  options.subscribeFriendList = config_.subscribeFriendList;
  options.log = config_.log;
  router_ = std::make_unique<SessionRouter::Router>(gamesTap_.get(), remotesTap_.get(), options);
  server_->Attach(router_.get());
  remotes_->Attach(router_.get());
}

SessionBridge::~SessionBridge() { Stop(); }

bool SessionBridge::SendToLogin(std::string_view frame) { return remotesTap_->SendToLogin(frame); }

std::optional<ConnectRequest> SessionBridge::BuildRequest(const SessionRouter::RemoteOpenRequest& request) {
  const Identity id = config_.identity ? config_.identity() : Identity();
  ConnectRequest out;
  out.url = config_.remoteUri;
  // The legacy-account route: percent-encoded discordid/password in the query, authorised by the public
  // server key. Same encoder the PC bridge uses; the password is never concatenated by hand.
  bool hasUrlCredentials = false;
  if (!id.discordId.empty() && !id.password.empty()) {
    std::optional<std::string> withCredentials =
        nevr_serverdb_uri::BuildBridgeCredentialUri(out.url, id.discordId, id.password);
    if (withCredentials.has_value()) {
      out.url = std::move(*withCredentials);
      hasUrlCredentials = true;
    } else if (config_.log) {
      config_.log(LogLevel::Error, "[bridge] could not percent-encode URL credentials; not using them");
    }
  }
  const std::string bearer = EvrCodec::SelectRemoteBearer(hasUrlCredentials, id.jwt, id.serverKey);
  if (bearer.empty()) {
    if (config_.log) {
      config_.log(LogLevel::Error,
                  "[bridge] remote=" + std::to_string(request.remote) +
                      " not started: neither an account JWT nor configured credentials (no unauthenticated session)");
    }
    return std::nullopt;
  }
  out.headers.push_back({"Authorization", "Bearer " + bearer});
  if (config_.log) {
    config_.log(LogLevel::Info, std::string("[bridge] remote=") + std::to_string(request.remote) +
                                    " auth route: " + (hasUrlCredentials ? "server key + URL credentials" : "account JWT"));
    if (!hasUrlCredentials && EvrCodec::IsBearerReplacingPath(out.url)) {
      config_.log(LogLevel::Warning,
                  "[bridge] the account JWT would go to a path whose front replaces the Bearer with the server key; "
                  "the login will arrive unauthenticated (use the /nevr ingress)");
    }
  }
  return out;
}

uint16_t SessionBridge::Start() {
  if (started_.load(std::memory_order_acquire)) return server_->port();
  if (!IsAcceptableRemoteUrl(config_.remoteUri)) {
    if (config_.log) {
      config_.log(LogLevel::Error, "[bridge] not started: the configured remote URI is not wss://");
    }
    return 0;
  }
  const uint16_t port = server_->Start();
  started_.store(port != 0, std::memory_order_release);
  return port;
}

void SessionBridge::Stop() {
  if (!started_.load(std::memory_order_acquire)) return;
  started_.store(false, std::memory_order_release);
  server_->Stop();    // reports OnGameClose for every upgraded connection
  remotes_->Stop();   // close frames, joins workers, no router callbacks
  router_->Shutdown();
}

}  // namespace quest_net
