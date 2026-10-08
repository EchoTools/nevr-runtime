// The Quest bridge as the sentinel runs it: quest_net::SessionBridge's composition (loopback game
// server -> shared session router -> remote WebSocket transport) with FrameTap decorators between
// the router and the two transports, and a side channel for the social facade's requests.
//
//   game --ws://127.0.0.1:<port>--> LoopbackGameServer --> Router --> ConnectorRemoteTransport --> service
//                                       ^ tap (server->game)   ^ tap (game->server)
//
// The route selection and the credentials handling are the same as SessionBridge::BuildRequest (the
// same shared functions: EvrCodec::SelectRemoteBearer, ServerDbUri::BuildBridgeCredentialUri); only the
// token-auth route is wired in production (jwt from the token session, no URL credentials).
//
// Login. The router is built WITHOUT a login builder. On Quest the game sends its own LoginRequest
// through libpnsovr's CNSUser::SendLogInRequest, and the login rewrite (quest/login) has already put
// the NEVR token, the NEVR account id and the game's HMD serial into it; the router relays it as the
// first frame of the login session. Injecting a second LoginRequest here would give the service two
// logins on one session and the game's own login state machine would still be waiting for its reply.
//
// The connector is injected so the host test can drive the whole path without libcurl or TLS; the
// production caller passes a quest_net::CurlWsConnector.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "quest/integration/frame_tap.h"
#include "quest/integration/tapped_transports.h"
#include "quest/net/loopback_game_server.h"
#include "quest/net/remote_ws.h"
#include "quest/net/session_bridge.h"
#include "runtime/compat/session_router.h"

namespace nevr_quest::integration {

class IntegratedBridge {
 public:
  struct Config {
    std::string remoteUri;                                  // wss://... community socket URI
    std::function<quest_net::Identity()> identity;          // called once per remote session
    quest_net::WsConnector* connector = nullptr;            // not owned; must outlive the bridge
    quest_net::LoopbackGameServer::Config loopback;
    SessionRouter::Limits limits;
    SessionRouter::LogSink log;
    FrameTapSinks tap;                                      // consumers of the relayed frames
  };

  explicit IntegratedBridge(Config config);
  ~IntegratedBridge();
  IntegratedBridge(const IntegratedBridge&) = delete;
  IntegratedBridge& operator=(const IntegratedBridge&) = delete;

  // Binds 127.0.0.1 on an ephemeral port. Returns the port, or 0 (logged) when it could not start or
  // the remote URI is not wss://.
  std::uint16_t Start();
  void Stop();
  std::uint16_t port() const { return server_->port(); }
  // The value the game must be redirected to: "ws://127.0.0.1:<port>/<token>/" with the per-start access
  // token the listener requires (quest_net::LoopbackGameServer::LoopbackUri). Never log it.
  std::string LoopbackUri() const { return server_->LoopbackUri(); }

  // A request toward the service on the login session (see TappedRemoteTransport::SendToLogin).
  bool SendToLogin(std::string_view frame) { return remotes_tap_->SendToLogin(frame); }

 private:
  std::optional<quest_net::ConnectRequest> BuildRequest(const SessionRouter::RemoteOpenRequest& request);

  Config config_;
  FrameTap tap_;
  std::unique_ptr<quest_net::ConnectorRemoteTransport> remotes_;
  std::unique_ptr<quest_net::LoopbackGameServer> server_;
  std::unique_ptr<TappedGameTransport> games_tap_;
  std::unique_ptr<TappedRemoteTransport> remotes_tap_;
  std::unique_ptr<SessionRouter::Router> router_;
  bool started_ = false;
};

}  // namespace nevr_quest::integration
