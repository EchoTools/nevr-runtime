#pragma once
// The EVR session router: the per-connection state machine that sits between the game's local
// connections and the community service's remote sessions (ADR 0003, contract 3).
//
// Platform neutral by construction: no Windows or Winsock headers, no sockets, no threads of its own,
// no TLS, no game state, no logging backend. Adapters supply the two transports and the login frame;
// the router owns the rules:
//
//   * connection identity: the Nth connection the game opens is config (0), login (1) or matchmaker
//     (2+). The login connection owns a remote session; matchmaker connections attach to that same
//     remote session (Nakama correlates a matchmaker allocation with the login session).
//   * login injection: exactly once per login session, before any frame the game queued while the
//     remote was still opening, from a caller-supplied builder (the router never sees a token).
//   * ordering: frames reach the remote in the order they arrived; the login request is first.
//   * remote end (close or error): every game socket on that session is closed and the session is
//     forgotten, so the game's next connection is a new login rather than a matchmaker.
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
inline constexpr uint16_t kCloseMessageTooBig = 1009;  // a frame over Limits::maxFrameBytes
inline constexpr uint16_t kCloseInternalError = 1011;  // the remote could not be started
inline constexpr uint16_t kCloseTryAgainLater = 1013;  // a bounded buffer overflowed

enum class SendResult {
  Sent,        // the transport took the frame
  WouldBlock,  // the transport is full: keep the frame, wait for OnGameWritable/OnRemoteWritable
  Failed,      // the endpoint is gone; the frame is lost
};

class GameTransport {
 public:
  virtual ~GameTransport() = default;
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

struct Limits {
  std::size_t maxFrameBytes = 4u * 1024u * 1024u;       // one WebSocket message, either direction
  std::size_t maxPendingFrames = 256;                   // game frames waiting for a remote to open
  std::size_t maxPendingBytes = 4u * 1024u * 1024u;
  std::size_t maxOutboundBytes = 4u * 1024u * 1024u;    // frames a full transport has not taken yet
};

struct Options {
  Limits limits;
  LoginFrameBuilder buildLogin;       // null: no injection (the game would send its own)
  bool subscribeFriendList = true;    // send the friend-list subscribe after LoginSuccess
  LogSink log;                        // null: logging off
};

struct Stats {
  std::size_t games = 0;
  std::size_t remotes = 0;
  std::size_t pendingFrames = 0;  // across all remotes
  std::size_t outboundBytes = 0;  // to remotes and to games
  int nextConnIdx = 0;
  uint64_t droppedGameFrames = 0;
  uint64_t droppedRemoteFrames = 0;
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
    Outbox out;
  };
  struct Remote {
    int ownerConn = -1;  // connIdx of the connection that created it
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
  Game* SharedRouteLocked(GameId* target);
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
  GameId activeGame_ = kNoGame;
  uint64_t droppedGameFrames_ = 0;
  uint64_t droppedRemoteFrames_ = 0;
  bool shutdown_ = false;
};

}  // namespace SessionRouter
