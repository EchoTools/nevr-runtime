#pragma once
// The EVR session router: the per-connection state machine that sits between the game's local
// connections and the community service's remote sessions (ADR 0003, contract 3).
//
// Platform neutral by construction: no Windows or Winsock headers, no sockets, no threads of its own,
// no TLS, no game state, no logging backend. Adapters supply the two transports and the login frame;
// the router owns the rules:
//
//   * connection identity: an adapter that knows a connection's role says so when the connection opens
//     (OpenHint: the PC bridge reads the request target and the listener). A hinted role is final: a first frame
//     that contradicts it is logged once at Warning and not acted on. Without a hint (Quest) a connection's role
//     is named by its FIRST data frame (ClassifyFirstFrame): SNSConfigRequestv2 is config, the lobby requests are
//     matchmaker, LogInRequestv2 is login. Until that frame arrives the role is provisional, by connection order:
//     the Nth connection is config (0), login (1) or matchmaker (2+), so the session can open before the game has
//     said anything. A first frame that corrects the provisional role relabels the connection IN PLACE on the
//     session it already shares: nothing is released, closed or re-attached.
//   * one session: config, login and matchmaker connections share ONE remote session (RemoteOpenRequest::session).
//     It is opened by the first config or login connection, and a matchmaker attaches to it once a login
//     connection has been attached (Nakama correlates a matchmaker allocation with the login session). A
//     matchmaker that arrives with no login connection yet, or with no session, keeps a remote of its own
//     (standaloneMatchmaker); that remote is never the session. A connection closing never ends the session; the
//     session ends when its remote ends (see "remote end").
//   * server-to-game routing on the session is by role, never to whichever socket spoke last:
//     replies to login-connection requests (and the settings that follow a login) go to the login
//     connection; SNSConfigSuccessv2 and SNSConfigFailurev2 go to the connection that sent the oldest open config
//     request with the reply's (type, id) (the service echoes the request's two strings), so a request the service
//     never answers does not shift later replies; a reply nobody asked for, or whose requester has closed, is
//     dropped, counted (Stats::configRepliesUnmatched, configRepliesOrphaned) and logged every time; lobby
//     traffic goes
//     to the newest matchmaker connection; and an STcpConnectionUnrequireEvent goes to the connection whose reply
//     it follows, only while that connection has a request outstanding (the game's own count wraps otherwise; see
//     TakeUnrequireLocked); one with nothing to lower is dropped.
//   * login injection: exactly once per session, from a caller-supplied builder (the router never sees a token),
//     when a login connection is attached to an OPEN session that has not sent one: at attach or at open,
//     whichever comes second. Frames of the connections attached to the session queue behind it (a config
//     connection's pass), and a closing connection's queued frames leave the queue with it.
//   * login removal notice (Options::loginRemovedJson, Quest): the game sends its own login and, when the login
//     session ends under it (a remote failure) with nothing outstanding on a login socket that is still
//     open, its login socket reconnects WITHOUT a new login (an established count-0 socket raises no Lost
//     event, so the game stays "logged in" on a session nobody logged into). Nothing is replayed and no
//     credential is kept: the router sends each game socket that reconnects, once, an SNSLoginRemovedNotify
//     for the account it last saw logged in (on its first frame, or when it has stayed silent: OnGameSilent).
//     It cannot tell the login socket from the others by order, so it sends to all of them: the game ignores
//     the notice on any peer but its login peer (CNSUser::LoginRemovedCB). The notice stays armed until the
//     game sends its own login or the next session answers LoginSuccess. The game's own handler puts a
//     logged-in game on the login-failed screen with RETRY, and RETRY runs the game's own login with a fresh
//     token. A login connection that ended with requests outstanding is the game's Lost path (-95, RETRY):
//     nothing is armed for it. What is kept is the 16-byte account id of the last LoginSuccess, cleared on any
//     LogOut, a rejected login and Shutdown.
//   * ordering: frames reach the remote in the order they arrived. An injected login request goes ahead of the
//     frames that waited for it, except a config connection's, which may precede it on a session already open.
//   * remote end (close or error): every game socket on that session, config included, is closed and the
//     session is forgotten, so the game's next connection is a new login rather than a matchmaker.
//   * a held session: while the wiring says the account the login needs is still being obtained
//     (Options::loginGate answers Awaiting) the session remote is not opened, whichever connection opened the
//     session, and the connections on it are neither failed nor closed; their frames queue. When the gate
//     answers Ready the remote opens and the queued frames follow; when it answers Refused the hold ends with a
//     close of every connection on the session (kCloseInternalError). A standalone matchmaker is never held.
//   * the login connection is silent until the game sends its LogInRequest, however long the player takes, so
//     the transport is told (GameTransport::SetIdleExempt) not to close it for sending nothing, held or not,
//     for as long as its role is login, provisional or confirmed. A relabel moves the exemption with the role: a
//     provisional login that proves to be config drops it, and the real login takes it once its first frame
//     names it.
//   * limits: an oversized frame, an unbounded wait for a remote that never opens and a transport that
//     never drains are each ended with a named close code and one log line, never silently dropped.
//
// Locking: one mutex guards the tables. The router NEVER calls a transport, the login builder or the log
// sink while holding it, so a transport whose Close() blocks on its own I/O thread (which may be calling
// back into the router) cannot deadlock the router. Sends to one endpoint are serialised through a
// per-endpoint queue and drained by whichever caller finds the queue idle, so frame order holds without
// the lock being held across a send.
//
// Nothing here logs a frame payload, a token, a password or a URL. Log lines carry ids, sizes, symbols
// and numeric codes only.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/socket_role.h"

namespace nevr_session_router {

using GameId = uint64_t;    // one accepted local connection; never reused within a Router
using RemoteId = uint64_t;  // one remote session; never reused within a Router
inline constexpr GameId kNoGame = 0;
inline constexpr RemoteId kNoRemote = 0;

// One role type for the router and the PC bridge's RoleTable.
using Role = nevr_socket_role::Role;
using nevr_socket_role::RoleName;

// What the adapter knows about a connection when it opens, before the game has sent a frame. PC knows the
// role from the request target (/config, /login) or the listener; Quest knows nothing and leaves `hinted`
// false. A hinted role is final: a first frame that contradicts it is logged, not acted on. The matchmaker cap
// counts a connection by its hint.
struct OpenHint {
  Role role = Role::Config;
  bool hinted = false;
};

// WebSocket close codes the router closes with (RFC 6455 7.4.1).
inline constexpr uint16_t kCloseGoingAway = 1001;      // the remote session ended; game should reconnect
inline constexpr uint16_t kClosePolicyViolation = 1008;  // a connection the router refuses to serve
inline constexpr uint16_t kCloseMessageTooBig = 1009;  // a frame over Limits::maxFrameBytes
inline constexpr uint16_t kCloseInternalError = 1011;  // the remote could not be started
inline constexpr uint16_t kCloseTryAgainLater = 1013;  // a bounded buffer overflowed

enum class SendResult {
  Sent,        // the transport took the frame
  WouldBlock,  // the transport is full: keep the frame, wait for OnGameWritable/OnRemoteWritable
  Failed,      // the endpoint is gone; the frame is lost
};

// The role a connection's first data frame names. LogInRequestv2 and any symbol that is not a config or
// lobby request name the login connection. Pure; the router applies it with one guard (see Router).
Role ClassifyFirstFrame(uint64_t symbol);
// True for the server-to-game messages that answer a request made on the login connection.
bool IsLoginSessionReply(uint64_t symbol);
// True for SNSConfigSuccessv2 and SNSConfigFailurev2: the answers to a config connection's request.
bool IsConfigSessionReply(uint64_t symbol);
// True when a message the game sends on a connection of role `role` raises the connection's
// outstanding-request count (the game sends it with the require flag). The service's
// STcpConnectionUnrequireEvent lowers it; the game's count is 8 bits and wraps below zero.
bool RequestRaisesRequireCount(Role role, uint64_t symbol);

class GameTransport {
 public:
  virtual ~GameTransport() = default;
  // This is the login connection: it is silent until the game's LogInRequest, so the transport must not
  // close it for sending nothing. `exempt == false` when the connection stops being the login connection.
  // Optional.
  virtual void SetIdleExempt(GameId game, bool exempt) {
    (void)game;
    (void)exempt;
  }
  virtual SendResult Send(GameId game, std::string_view frame, bool binary) = 0;
  // Must not block on the router. May be called from any thread, never with the router lock held.
  virtual void Close(GameId game, uint16_t code, std::string_view reason) = 0;
};

struct RemoteOpenRequest {
  RemoteId remote = kNoRemote;
  int connIdx = -1;
  Role role = Role::Config;
  // True when a matchmaker connection found no session with a login connection to share and needs its own
  // remote. Such a remote is never the session.
  bool standaloneMatchmaker = false;
  // True when this remote is the session remote that config, login and matchmaker connections share. The
  // opener's role is `role`, which may be config or login.
  bool session = false;
};

class RemoteTransport {
 public:
  virtual ~RemoteTransport() = default;
  // Begins an asynchronous connect. Events come back through Router::OnRemote*. False means the connect
  // could not even be started (bad URL, TLS policy refused); the router ends the session.
  virtual bool Open(const RemoteOpenRequest& request) = 0;
  virtual SendResult Send(RemoteId remote, std::string_view frame, bool binary) = 0;
  virtual void Close(RemoteId remote, uint16_t code) = 0;
};

enum class LogLevel { Debug, Info, Warning, Error };
using LogSink = std::function<void(LogLevel level, const std::string& line)>;

// Produces the complete EVR LoginRequest message for the login session, or nullopt when the identity is
// not available (no token, no configured credentials). The router never fabricates one.
using LoginFrameBuilder = std::function<std::optional<std::string>()>;

// Whether the account a login needs is available. Awaiting: not yet (the player has not signed in); the
// session is held. Ready: it is. Refused: it will not be (the sign-in failed for good).
enum class LoginGate { Ready, Awaiting, Refused };
// Called with the router lock held, from OnGameOpen/OnGameFrame/ReevaluateHeldLogins: it must be a lock-free
// read of a flag the wiring keeps current (never the token session itself), and must not call the router.
using LoginGateFn = std::function<LoginGate()>;

struct Limits {
  std::size_t maxFrameBytes = 4u * 1024u * 1024u;       // one WebSocket message, either direction
  std::size_t maxPendingFrames = 256;                   // game frames waiting for a remote to open
  std::size_t maxPendingBytes = 4u * 1024u * 1024u;
  std::size_t maxOutboundBytes = 4u * 1024u * 1024u;    // frames a full transport has not taken yet
  std::size_t maxMatchmakerConnections = 8;             // live matchmaker connections (counted by role, hinted or not)
};

// Injection is OFF unless the wiring turns it on, and is mutually exclusive with a game that sends its own
// login. The PC bridge injects (pnsrad sends no login: it has no identity). On Quest the game's own login is
// rewritten in place (PR #221) and is the only login, so the Quest wiring leaves both fields at their
// defaults and the router sends no frame the game did not send.
struct Options {
  Limits limits;
  LoginFrameBuilder buildLogin;       // null (default): no login injection; the game sends its own
  LoginGateFn loginGate;              // null (default): the account is always available (PC, tests)
  bool subscribeFriendList = false;   // true: send a friend-list subscribe after LoginSuccess (PC only)
  // Non-empty: the JSON document of the SNSLoginRemovedNotify sent to a game whose login session was lost
  // under it (Quest). Built by the wiring (nlohmann::json); empty (default) sends no notice (PC).
  std::string loginRemovedJson;
  LogSink log;                        // null: logging off
};

struct Stats {
  std::size_t games = 0;
  std::size_t remotes = 0;
  std::size_t pendingFrames = 0;  // across all remotes
  std::size_t outboundBytes = 0;  // to remotes and to games
  std::size_t heldRemotes = 0;    // session remotes waiting for the account (not opened yet)
  int nextConnIdx = 0;
  uint64_t droppedGameFrames = 0;
  uint64_t droppedRemoteFrames = 0;
  uint64_t loginsRemoved = 0;      // SNSLoginRemovedNotify sent to a game that reconnected without a login
  bool loginUserKnown = false;     // the account of the last LoginSuccess is held (never a credential)
  bool loginRemovedDue = false;    // armed: each reconnecting socket is sent the notice once, until the game logs in
  uint64_t droppedUnrequires = 0;  // Unrequires with no request outstanding to lower, or whose message was dropped
  // Unrequires inside a frame whose connection had nothing outstanding to lower. They cannot be removed from the
  // frame, so they reach the game (or are dropped with the frame) and wrap its count: counted, not prevented.
  uint64_t unmatchedEmbeddedUnrequires = 0;
  // Config replies whose requesting socket had closed and that no live socket asked for, dropped.
  uint64_t configRepliesOrphaned = 0;
  // Config replies no open request asked for (its (type, id) matches none), dropped.
  uint64_t configRepliesUnmatched = 0;
  // Unanswered config requests dropped from the list of requests awaiting a reply, oldest first, because it was
  // full (a request the service never answers stays in the list until then).
  uint64_t configRequestsEvicted = 0;
};

class Router {
 public:
  Router(GameTransport* games, RemoteTransport* remotes, Options options);
  ~Router();
  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;

  // ---- events from the game side (the loopback listener) ----------------------------------------
  // `hint.hinted`: the adapter names the role and it is final. Otherwise the role is provisional (by order).
  void OnGameOpen(GameId game, OpenHint hint = OpenHint());
  void OnGameFrame(GameId game, std::string frame, bool binary);
  void OnGameClose(GameId game);
  void OnGameWritable(GameId game);
  // The connection has been upgraded for the wiring's silent period and has sent no data frame: the login
  // socket of a game that reconnected without a new login is such a connection, while a config or matchmaker
  // socket sends its request at once. Idempotent; ignored for a connection that has spoken.
  void OnGameSilent(GameId game);

  // ---- events from the remote side ---------------------------------------------------------------
  void OnRemoteOpen(RemoteId remote);
  void OnRemoteFrame(RemoteId remote, std::string frame, bool binary);
  void OnRemoteClose(RemoteId remote, uint16_t code);
  // The remote failed (connect refused, TLS verification failed, I/O error). `status` is an HTTP status
  // or 0; `what` is a short fixed description, never a URL or header.
  void OnRemoteError(RemoteId remote, int status, std::string_view what);
  void OnRemoteWritable(RemoteId remote);

  // Re-reads Options::loginGate for every held session: Ready opens the remote, Refused ends the hold with a
  // close of every connection on it (kCloseInternalError). Call it whenever the wiring changes the gate's answer. Idempotent, cheap
  // when nothing is held, safe from any thread.
  void ReevaluateHeldLogins();

  // Ends every session and closes every socket. Idempotent.
  void Shutdown();

  Stats GetStats() const;

 private:
  struct Item {
    std::shared_ptr<const std::string> data;  // shared so a drainer can send without holding the lock
    bool binary = true;
    GameId game = kNoGame;  // the connection that sent it (Remote::pending only): its frames leave with it
  };
  struct Outbox {
    std::deque<Item> queue;
    std::size_t bytes = 0;
    bool draining = false;
    bool blocked = false;
  };
  struct Game {
    int connIdx = -1;
    Role role = Role::Config;
    RemoteId remote = kNoRemote;  // kNoRemote once its session ended
    bool closing = false;         // a close was issued; waiting for the transport's OnGameClose
    bool classified = false;      // the first data frame has named the role
    bool hinted = false;          // the adapter named the role at open: it is final
    uint32_t required = 0;        // requests sent on the shared login session that await their Unrequire
    bool noticeSent = false;      // it was sent the login-removed notice (never twice)
    Outbox out;
  };
  struct Remote {
    int ownerConn = -1;  // connIdx of the connection that created it
    RemoteOpenRequest request;  // what Open was (or will be) called with
    bool deferred = false;      // a held session: Open has not been called yet
    bool open = false;
    bool loginPending = false;  // login builder running: frames of non-config connections queue behind it
    bool loginSent = false;
    bool loginAttached = false;  // a login connection has been attached (never cleared): a matchmaker may share
    // Game frames that arrived before the remote opened or while the login is being built, in arrival order.
    std::deque<Item> pending;
    std::size_t pendingBytes = 0;
    Outbox out;
  };
  struct Effects;

  // All of these require mutex_ held (suffix Locked) and only record work in `fx`.
  void FailSessionLocked(RemoteId remote, uint16_t code, const char* why, bool closeRemote, Effects& fx);
  // `from` is the game whose frame this is (kNoGame for a frame the router made). A config request from a game
  // joins the session's list of open config requests, with its (type, id).
  bool PushToRemoteLocked(RemoteId remote, Remote& r, std::shared_ptr<const std::string> data, bool binary,
                          GameId from, Effects& fx);
  void FlushOpenLocked(RemoteId remote, Remote& r, std::optional<std::string> login, Effects& fx);
  void CloseGameLocked(GameId game, uint16_t code, const char* why, Effects& fx);
  // Gives `g` (whose connIdx and role are set) its remote: the session, or a remote of its own.
  void AttachLocked(GameId game, Game& g, Effects& fx);
  // Creates a remote for `g`; `session` makes it the session remote.
  void OpenRemoteLocked(GameId game, Game& g, bool session, Effects& fx);
  // Makes `game` the session's login connection; the previous one, if still open, rides it as a matchmaker.
  void TakeOverLoginLocked(GameId game, Game& g, Effects& fx);
  // Starts a login injection when a login connection is attached to the open session and none was sent.
  void MaybeInjectLocked(Effects& fx);
  bool LoginAttachedLocked(RemoteId remote) const;
  // Removes the frames `game` queued on the remotes' pending queues (never an outbox).
  void PurgePendingLocked(GameId game);
  bool GateAwaitingLocked() const;
  Role ProvisionalRoleLocked() const;
  void ClassifyGameLocked(GameId game, Game& g, uint64_t symbol, Effects& fx);
  // Sends the removal notice to `game` once, when it reconnected after the loss the notice is due for.
  // The router cannot tell the login socket from the others by order or silence, so every socket that
  // reconnected gets it: the game drops it on any peer but its login peer (CNSUser::LoginRemovedCB).
  void SendLoginRemovedLocked(GameId game, Game& g, Effects& fx);
  void ForgetLoginUserLocked();
  void RecomputeActiveLocked();
  bool OnSessionLocked(GameId id) const;
  GameId RouteSessionFrameLocked(const std::string& frame, bool* quietDrop, bool* logged, Effects& fx);
  bool TakeUnrequireLocked(GameId target);
  void CountRequirementsLocked(Game& g, const std::string& frame);
  std::size_t LiveMatchmakersLocked() const;
  void FailSession(RemoteId remote, uint16_t code, const char* why, bool closeRemote);
  void Log(Effects& fx, LogLevel level, std::string line);

  void Run(Effects& fx);  // executes recorded work with the mutex released
  void BuildAndSendLogin(RemoteId remote);  // the builder runs here, with no lock held
  void DrainRemote(RemoteId remote);
  void DrainGame(GameId game);

  GameTransport* games_;
  RemoteTransport* remotes_;
  Options options_;

  mutable std::mutex mutex_;
  std::unordered_map<GameId, Game> gameTable_;
  std::unordered_map<RemoteId, Remote> remoteTable_;
  RemoteId nextRemote_ = 1;
  int connectionCount_ = 0;
  RemoteId sessionRemote_ = kNoRemote;  // the remote config, login and matchmaker connections share
  GameId loginGame_ = kNoGame;          // the session's login connection (provisional or confirmed)
  GameId activeGame_ = kNoGame;         // the newest matchmaker connection on the session
  // The config requests that went to the session remote and have no reply yet, oldest first, each with the
  // connection that sent it and the (type, id) the service will echo in its reply.
  struct ConfigRequestRecord {
    GameId owner = kNoGame;
    std::optional<nevr_evr_codec::ConfigKey> key;  // the request's (type, id); nullopt when its payload is unreadable
  };
  std::deque<ConfigRequestRecord> configRequests_;
  uint64_t configRepliesOrphaned_ = 0;   // the requester had closed
  uint64_t configRepliesUnmatched_ = 0;  // no open request had the reply's (type, id)
  uint64_t configRequestsEvicted_ = 0;   // the oldest unanswered request, dropped to bound the list
  // The connections that owe an Unrequire for the messages the service paired with one, in the order those
  // messages went out (kNoGame: the message was dropped, so its Unrequire is too).
  std::deque<GameId> owedUnrequires_;
  uint64_t droppedUnrequires_ = 0;
  uint64_t loginsRemoved_ = 0;
  // The account of the last LoginSuccess on the login session (not a credential), and whether the notice is
  // due for the next login connection. loginRemovedJson only.
  bool haveLoginUser_ = false;
  nevr_evr_codec::UserId loginUser_;
  bool removalDue_ = false;      // armed by a silent loss; disarmed by the game's own login or the next LoginSuccess
  uint64_t unmatchedEmbeddedUnrequires_ = 0;
  uint64_t droppedGameFrames_ = 0;
  uint64_t droppedRemoteFrames_ = 0;
  bool shutdown_ = false;
};

}  // namespace nevr_session_router
