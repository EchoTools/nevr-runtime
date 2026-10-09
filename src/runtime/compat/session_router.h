#pragma once
// The EVR session router: the per-connection state machine that sits between the game's local
// connections and the community service's remote sessions (ADR 0003, contract 3).
//
// Platform neutral by construction: no Windows or Winsock headers, no sockets, no threads of its own,
// no TLS, no game state, no logging backend. Adapters supply the two transports and the login frame;
// the router owns the rules:
//
//   * connection identity: a connection's role is named by its FIRST data frame (ClassifyFirstFrame):
//     SNSConfigRequestv2 is config, the lobby requests are matchmaker, LogInRequestv2 is login. Until that
//     frame arrives the role is provisional, by connection order: the Nth connection is config (0), login
//     (1) or matchmaker (2+), so the remote can open before the game has said anything. The login
//     connection owns a remote session; matchmaker connections attach to that same remote session (Nakama
//     correlates a matchmaker allocation with the login session). A first frame that contradicts the
//     provisional role moves the connection to the role the frame names.
//   * server-to-game routing on the shared login session is by role, never to whichever socket spoke last:
//     replies to login-connection requests (and the settings that follow a login) go to the login
//     connection, lobby traffic to the newest matchmaker connection, and an STcpConnectionUnrequireEvent to
//     the connection whose reply it follows, only while that connection has a request outstanding (the game's
//     own count wraps otherwise; see TakeUnrequireLocked); one with nothing to lower is dropped.
//   * login injection: exactly once per login session, before any frame the game queued while the
//     remote was still opening, from a caller-supplied builder (the router never sees a token).
//   * ordering: frames reach the remote in the order they arrived; the login request is first.
//   * remote end (close or error): every game socket on that session is closed and the session is
//     forgotten, so the game's next connection is a new login rather than a matchmaker.
//   * a held login: while the wiring says the account the login needs is still being obtained
//     (Options::loginGate answers Awaiting) the login connection's remote is not opened, and the connection
//     is neither failed nor closed. When the gate
//     answers Ready the remote opens and the frames the game queued meanwhile follow; when it answers Refused
//     the hold ends with a close. Config and matchmaker connections are never held: with no account they fail
//     at once.
//   * the login connection is silent until the game sends its LogInRequest, however long the player takes, so
//     the transport is told (GameTransport::SetIdleExempt) not to close it for sending nothing, held or not,
//     for as long as it is the login connection.
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

namespace SessionRouter {

using GameId = uint64_t;    // one accepted local connection; never reused within a Router
using RemoteId = uint64_t;  // one remote session; never reused within a Router
inline constexpr GameId kNoGame = 0;
inline constexpr RemoteId kNoRemote = 0;

enum class Role { Config, Login, Matchmaker };
const char* RoleName(Role role);

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
  // True when a matchmaker connection found no login session to share and needs its own remote.
  bool standaloneMatchmaker = false;
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
// login connection is held. Ready: it is. Refused: it will not be (the sign-in failed for good).
enum class LoginGate { Ready, Awaiting, Refused };
// Called with the router lock held, from OnGameOpen/OnGameFrame/ReevaluateHeldLogins: it must be a lock-free
// read of a flag the wiring keeps current (never the token session itself), and must not call the router.
using LoginGateFn = std::function<LoginGate()>;

struct Limits {
  std::size_t maxFrameBytes = 4u * 1024u * 1024u;       // one WebSocket message, either direction
  std::size_t maxPendingFrames = 256;                   // game frames waiting for a remote to open
  std::size_t maxPendingBytes = 4u * 1024u * 1024u;
  std::size_t maxOutboundBytes = 4u * 1024u * 1024u;    // frames a full transport has not taken yet
  std::size_t maxMatchmakerConnections = 8;             // live matchmaker connections sharing the login session
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
  LogSink log;                        // null: logging off
};

struct Stats {
  std::size_t games = 0;
  std::size_t remotes = 0;
  std::size_t pendingFrames = 0;  // across all remotes
  std::size_t outboundBytes = 0;  // to remotes and to games
  std::size_t heldRemotes = 0;    // login remotes waiting for the account (not opened yet)
  int nextConnIdx = 0;
  uint64_t droppedGameFrames = 0;
  uint64_t droppedRemoteFrames = 0;
  uint64_t droppedUnrequires = 0;  // Unrequires with no request outstanding to lower, or whose message was dropped
};

class Router {
 public:
  Router(GameTransport* games, RemoteTransport* remotes, Options options);
  ~Router();
  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;

  // ---- events from the game side (the loopback listener) ----------------------------------------
  void OnGameOpen(GameId game);
  void OnGameFrame(GameId game, std::string frame, bool binary);
  void OnGameClose(GameId game);
  void OnGameWritable(GameId game);

  // ---- events from the remote side ---------------------------------------------------------------
  void OnRemoteOpen(RemoteId remote);
  void OnRemoteFrame(RemoteId remote, std::string frame, bool binary);
  void OnRemoteClose(RemoteId remote, uint16_t code);
  // The remote failed (connect refused, TLS verification failed, I/O error). `status` is an HTTP status
  // or 0; `what` is a short fixed description, never a URL or header.
  void OnRemoteError(RemoteId remote, int status, std::string_view what);
  void OnRemoteWritable(RemoteId remote);

  // Re-reads Options::loginGate for every held login: Ready opens the remote, Refused ends the hold with a
  // close (kCloseInternalError). Call it whenever the wiring changes the gate's answer. Idempotent, cheap
  // when nothing is held, safe from any thread.
  void ReevaluateHeldLogins();

  // Ends every session and closes every socket. Idempotent.
  void Shutdown();

  Stats GetStats() const;

 private:
  struct Item {
    std::shared_ptr<const std::string> data;  // shared so a drainer can send without holding the lock
    bool binary = true;
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
    uint32_t required = 0;        // requests sent on the shared login session that await their Unrequire
    Outbox out;
  };
  struct Remote {
    int ownerConn = -1;  // connIdx of the connection that created it
    RemoteOpenRequest request;  // what Open was (or will be) called with
    bool deferred = false;      // a held login: Open has not been called yet
    bool open = false;
    bool loginPending = false;  // open event seen, login builder running
    bool loginSent = false;
    std::deque<Item> pending;  // game frames that arrived before the remote opened, in arrival order
    std::size_t pendingBytes = 0;
    Outbox out;
  };
  struct Effects;

  // All of these require mutex_ held (suffix Locked) and only record work in `fx`.
  void FailSessionLocked(RemoteId remote, uint16_t code, const char* why, bool closeRemote, Effects& fx);
  bool PushToRemoteLocked(RemoteId remote, Remote& r, std::shared_ptr<const std::string> data, bool binary,
                          Effects& fx);
  void FlushOpenLocked(RemoteId remote, Remote& r, std::optional<std::string> login, Effects& fx);
  void CloseGameLocked(GameId game, uint16_t code, const char* why, Effects& fx);
  void AttachLocked(GameId game, Game& g, Effects& fx);
  bool GateAwaitingLocked() const;
  Role ProvisionalRoleLocked() const;
  void ClassifyGameLocked(GameId game, Game& g, uint64_t symbol, Effects& fx);
  bool ReleaseRemoteLocked(GameId game, Game& g, Effects& fx);
  void RecomputeActiveLocked();
  bool OnLoginSessionLocked(GameId id) const;
  GameId RouteLoginSessionFrameLocked(const std::string& frame, bool* quietDrop);
  bool TakeUnrequireLocked(GameId target);
  void CountRequirementsLocked(Game& g, const std::string& frame);
  std::size_t LiveMatchmakersLocked() const;
  void FailSession(RemoteId remote, uint16_t code, const char* why, bool closeRemote);
  void Log(Effects& fx, LogLevel level, std::string line);

  void Run(Effects& fx);  // executes recorded work with the mutex released
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
  RemoteId loginRemote_ = kNoRemote;
  GameId loginGame_ = kNoGame;
  GameId activeGame_ = kNoGame;   // the newest matchmaker connection on the login session
  // The connections that owe an Unrequire for the messages the service paired with one, in the order those
  // messages went out (kNoGame: the message was dropped, so its Unrequire is too).
  std::deque<GameId> owedUnrequires_;
  uint64_t droppedUnrequires_ = 0;
  uint64_t droppedGameFrames_ = 0;
  uint64_t droppedRemoteFrames_ = 0;
  bool shutdown_ = false;
};

}  // namespace SessionRouter
