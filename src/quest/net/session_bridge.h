#pragma once
// The Quest counterpart of the PC ws_bridge: one object that owns the loopback game server, the shared
// session router, the remote transport and the verified-TLS WebSocket connector, wired together.
//
//   game --ws://127.0.0.2:<port>/<token>/--> LoopbackGameServer --> Router --> ConnectorRemoteTransport
//                                                                 --wss (verified TLS)--> service
//
// Redirect: nevr_cfg::ResolveRedirect (src/runtime/lifecycle/service_redirect.h) decides WHETHER a game URL
// is redirected, but with bridgeActive=true it returns the bare "ws://127.0.0.1:<port>", which the game
// never reaches (libr15 dials a non-loopback interface for 127.0.0.1; see loopback_game_server.h) and which
// carries no access token. The wiring that installs the redirect must use LocalUri() as the replacement
// value for such a URL. Nothing here installs a hook: hook activation is gated by ADR 0003.
//
// Login: the bridge injects NOTHING. On Quest the game's own login (rewritten in place, PR #221) is the only
// login, so Config has no login builder and the router runs with its injection options at their defaults.
// The game's LoginRequest is relayed as the first frame of the login session; a second one here would give
// the service two logins on one session.
//
// A held login. While the player has not signed in there is no account token. The login connection (the
// game's long-lived, silent one) must not fail then: Config::loginGate says whether the account is
// available, and while it answers Awaiting the router keeps the login connection open without a remote
// (and the loopback server exempts it from the idle close). The owner calls ReevaluateLoginGate() when
// the gate's answer changes: Ready opens the remote and the game's login goes through, Refused closes
// the connection. A config or matchmaker connection with no token still fails at once.
//
// Frame tap and side channel (the social facade): Config::tap sees every frame the bridge relays, in both
// directions, and the moment the service accepts the login (a server-to-game LoginSuccess). SendToLogin()
// sends one request on the login session outside the router's queue; it refuses until that LoginSuccess,
// because a request before it would reach the service unauthenticated.
//
// Identity: the bridge reads the credentials for each remote session from the caller's IdentityProvider
// when the session starts, mirrors the PC route selection (nevr_evr_codec::SelectRemoteBearer), and never
// stores, logs or caches a value. With neither a JWT nor URL credentials the session is not started.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "quest/net/curl_ws_connector.h"
#include "quest/net/frame_tap.h"
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
    CurlWsConnector::Config tls;                            // CA store; verification itself is not configurable
    // The WebSocket connector. Null: the bridge builds the verified-TLS CurlWsConnector from `tls`. Not
    // owned; must outlive the bridge. A host test passes a fake to drive the whole path without TLS.
    WsConnector* connector = nullptr;
    LoopbackGameServer::Config loopback;
    nevr_session_router::Limits limits;
    // Send the friend-list subscribe once the service accepts the login. The Quest game never asks for the
    // NEVR friend list itself (its own friend code talks to the Oculus platform, which the social facade
    // replaces), so the bridge asks, exactly as the PC bridge does after LoginSuccess.
    bool subscribeFriendList = false;
    nevr_session_router::LogSink log;
    // Whether the account the login needs is available (see "A held login"). Null: always available.
    // Called with the router lock held: a lock-free read of a flag the owner keeps current.
    nevr_session_router::LoginGateFn loginGate;
    FrameTapSinks tap;                                      // consumers of the relayed frames
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
  // The redirect value for the game: carries the per-start access token (see LoopbackGameServer).
  std::string LocalUri() const { return server_->LoopbackUri(); }

  // The loginGate's answer changed: opens or closes the held login connection. Cheap when nothing is held;
  // safe from any thread (the token-auth poll thread calls it).
  void ReevaluateLoginGate() { router_->ReevaluateHeldLogins(); }
  // Unrequires the router dropped because the connection had nothing outstanding.
  std::uint64_t DroppedUnrequires() const { return router_->GetStats().droppedUnrequires; }
  // Unrequires inside a frame that the connection had nothing outstanding for; they reach the game anyway.
  std::uint64_t UnmatchedEmbeddedUnrequires() const { return router_->GetStats().unmatchedEmbeddedUnrequires; }
  // Login connections currently held for the account (0 or 1 in practice).
  std::size_t HeldLogins() const { return router_->GetStats().heldRemotes; }
  // One whole EVR frame, toward the service, on the login session. False when there is no login session,
  // the login has not been accepted yet, or the transport refused it. Safe from any thread.
  bool SendToLogin(std::string_view frame);

 private:
  std::optional<ConnectRequest> BuildRequest(const nevr_session_router::RemoteOpenRequest& request);

  class TappedGames;
  class TappedRemotes;

  Config config_;
  FrameTap tap_;
  std::unique_ptr<CurlWsConnector> ownedConnector_;  // when Config::connector is null
  std::unique_ptr<ConnectorRemoteTransport> remotes_;
  std::unique_ptr<LoopbackGameServer> server_;
  std::unique_ptr<TappedGames> gamesTap_;
  std::unique_ptr<TappedRemotes> remotesTap_;
  std::unique_ptr<nevr_session_router::Router> router_;
  std::atomic<bool> started_{false};  // Start/Stop are the owner's to call (see LoopbackGameServer)
};

}  // namespace quest_net
