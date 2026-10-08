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

SessionBridge::SessionBridge(Config config)
    : config_(std::move(config)), connector_(WithLog(config_.tls, config_.log)) {
  ConnectorRemoteTransport::Config remoteConfig;
  remoteConfig.log = config_.log;
  remotes_ = std::make_unique<ConnectorRemoteTransport>(
      &connector_, [this](const SessionRouter::RemoteOpenRequest& r) { return BuildRequest(r); }, remoteConfig);
  server_ = std::make_unique<LoopbackGameServer>(WithLog(config_.loopback, config_.log));
  SessionRouter::Options options;
  options.limits = config_.limits;
  options.buildLogin = config_.buildLogin;
  options.log = config_.log;
  router_ = std::make_unique<SessionRouter::Router>(server_.get(), remotes_.get(), options);
  server_->Attach(router_.get());
  remotes_->Attach(router_.get());
}

SessionBridge::~SessionBridge() { Stop(); }

std::optional<ConnectRequest> SessionBridge::BuildRequest(const SessionRouter::RemoteOpenRequest& request) {
  const Identity id = config_.identity ? config_.identity() : Identity();
  ConnectRequest out;
  out.url = config_.remoteUri;
  // The legacy-account route: percent-encoded discordid/password in the query, authorised by the public
  // server key. Same encoder the PC bridge uses; the password is never concatenated by hand.
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

uint16_t SessionBridge::Start() {
  if (started_) return server_->port();
  if (!IsAcceptableRemoteUrl(config_.remoteUri)) {
    if (config_.log) {
      config_.log(LogLevel::Error, "[bridge] not started: the configured remote URI is not wss://");
    }
    return 0;
  }
  const uint16_t port = server_->Start();
  started_ = port != 0;
  return port;
}

void SessionBridge::Stop() {
  if (!started_) return;
  started_ = false;
  server_->Stop();    // reports OnGameClose for every upgraded connection
  remotes_->Stop();   // close frames, joins workers, no router callbacks
  router_->Shutdown();
}

}  // namespace quest_net
