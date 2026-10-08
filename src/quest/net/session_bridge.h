#pragma once
// The Quest counterpart of the PC ws_bridge: one object that owns the loopback game server, the shared
// session router, the remote transport and the verified-TLS WebSocket connector, wired together.
//
//   game --ws://127.0.0.1:<port>--> LoopbackGameServer --> Router --> ConnectorRemoteTransport
//                                                                 --wss (verified TLS)--> service
//
// The redirect that points the game at LocalUri() is the shared nevr_cfg::ResolveRedirect with
// bridgeActive=true and bridgePort=port() (src/runtime/lifecycle/service_redirect.h); this class only
// reports the port. Nothing here installs a hook: hook activation is gated by ADR 0003.
//
// Identity: the bridge reads the credentials for each remote session from the caller's IdentityProvider
// when the session starts, mirrors the PC route selection (EvrCodec::SelectRemoteBearer), and never
// stores, logs or caches a value. With neither a JWT nor URL credentials the session is not started.

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "quest/net/curl_ws_connector.h"
#include "quest/net/loopback_game_server.h"
#include "quest/net/remote_ws.h"
#include "runtime/compat/session_router.h"

namespace quest_net {

struct Identity {
  std::string jwt;        // Nakama account JWT (token-auth route)
  std::string serverKey;  // public socket server key; used only with URL credentials
  std::string discordId;  // configured legacy account (URL credentials route)
  std::string password;
};

class SessionBridge {
 public:
  struct Config {
    std::string remoteUri;                                  // wss://... community socket URI
    std::function<Identity()> identity;                     // called once per remote session
    SessionRouter::LoginFrameBuilder buildLogin;            // the complete EVR LoginRequest message
    CurlWsConnector::Config tls;                            // CA store; verification itself is not configurable
    LoopbackGameServer::Config loopback;
    SessionRouter::Limits limits;
    SessionRouter::LogSink log;
  };

  explicit SessionBridge(Config config);
  ~SessionBridge();
  SessionBridge(const SessionBridge&) = delete;
  SessionBridge& operator=(const SessionBridge&) = delete;

  // Starts the loopback listener. Returns its port, or 0 (logged) when it could not start or the
  // configured remote URI is not wss://.
  uint16_t Start();
  void Stop();
  uint16_t port() const { return server_->port(); }
  std::string LocalUri() const { return "ws://127.0.0.1:" + std::to_string(server_->port()); }

 private:
  std::optional<ConnectRequest> BuildRequest(const SessionRouter::RemoteOpenRequest& request);

  Config config_;
  CurlWsConnector connector_;
  std::unique_ptr<ConnectorRemoteTransport> remotes_;
  std::unique_ptr<LoopbackGameServer> server_;
  std::unique_ptr<SessionRouter::Router> router_;
  bool started_ = false;
};

}  // namespace quest_net
