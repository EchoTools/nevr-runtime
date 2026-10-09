#include "quest/integration/integrated_bridge.h"

#include <utility>

#include "runtime/compat/evr_codec.h"
#include "runtime/server/serverdb_uri.h"

namespace nevr_quest::integration {

using SessionRouter::LogLevel;

IntegratedBridge::IntegratedBridge(Config config)
    : config_(std::move(config)), tap_(FrameTapSinks{}) {
  // The login is accepted when the service's LoginSuccess passes the tap: the side channel opens, and the
  // configured consumer (the social facade's local account) hears about it.
  FrameTapSinks sinks = config_.tap;
  std::function<void(std::uint64_t)> consumer = std::move(sinks.onLoginSuccess);
  sinks.onLoginSuccess = [this, consumer](std::uint64_t account) {
    if (remotes_tap_) remotes_tap_->MarkLoginAccepted();
    if (consumer) consumer(account);
  };
  tap_ = FrameTap(std::move(sinks));

  quest_net::ConnectorRemoteTransport::Config remoteConfig;
  remoteConfig.log = config_.log;
  remotes_ = std::make_unique<quest_net::ConnectorRemoteTransport>(
      config_.connector, [this](const SessionRouter::RemoteOpenRequest& r) { return BuildRequest(r); },
      remoteConfig);

  quest_net::LoopbackGameServer::Config loopback = config_.loopback;
  if (!loopback.log) loopback.log = config_.log;
  server_ = std::make_unique<quest_net::LoopbackGameServer>(std::move(loopback));

  games_tap_ = std::make_unique<TappedGameTransport>(server_.get(), &tap_);
  remotes_tap_ = std::make_unique<TappedRemoteTransport>(remotes_.get(), &tap_);

  SessionRouter::Options options;
  options.limits = config_.limits;
  options.buildLogin = nullptr;  // the game's own rewritten LoginRequest is the login (see the header)
  options.subscribeFriendList = config_.subscribeFriendList;
  options.log = config_.log;
  router_ = std::make_unique<SessionRouter::Router>(games_tap_.get(), remotes_tap_.get(), options);
  server_->Attach(router_.get());
  remotes_->Attach(router_.get());
}

IntegratedBridge::~IntegratedBridge() { Stop(); }

std::optional<quest_net::ConnectRequest> IntegratedBridge::BuildRequest(
    const SessionRouter::RemoteOpenRequest& request) {
  const quest_net::Identity id = config_.identity ? config_.identity() : quest_net::Identity();
  quest_net::ConnectRequest out;
  out.url = config_.remoteUri;
  // The legacy-account route (same encoder the PC bridge uses; a password is never concatenated by hand).
  bool hasUrlCredentials = false;
  if (!id.discordId.empty() && !id.password.empty()) {
    std::optional<std::string> withCredentials =
        ServerDbUri::BuildBridgeCredentialUri(out.url, id.discordId, id.password);
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

std::uint16_t IntegratedBridge::Start() {
  if (started_) return server_->port();
  if (!quest_net::IsAcceptableRemoteUrl(config_.remoteUri)) {
    if (config_.log) config_.log(LogLevel::Error, "[bridge] not started: the configured remote URI is not wss://");
    return 0;
  }
  const std::uint16_t port = server_->Start();
  started_ = port != 0;
  return port;
}

void IntegratedBridge::Stop() {
  if (!started_) return;
  started_ = false;
  server_->Stop();   // reports OnGameClose for every upgraded connection
  remotes_->Stop();  // close frames, joins workers, no router callbacks
  router_->Shutdown();
}

}  // namespace nevr_quest::integration
