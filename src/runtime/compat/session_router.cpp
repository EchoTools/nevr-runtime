#include "runtime/compat/session_router.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <utility>

#include "runtime/compat/evr_codec.h"

namespace nevr_session_router {

namespace {

// Printf into a std::string. Only ever called with ids, sizes and fixed text.
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
std::string Fmt(const char* format, ...) {
  char stack[256];
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int needed = std::vsnprintf(stack, sizeof(stack), format, args);
  va_end(args);
  std::string out;
  if (needed < 0) {
    out = "(log format error)";
  } else if (static_cast<std::size_t>(needed) < sizeof(stack)) {
    out.assign(stack, static_cast<std::size_t>(needed));
  } else {
    out.resize(static_cast<std::size_t>(needed) + 1);
    std::vsnprintf(&out[0], out.size(), format, copy);
    out.resize(static_cast<std::size_t>(needed));
  }
  va_end(copy);
  return out;
}

unsigned long long Ull(uint64_t v) { return static_cast<unsigned long long>(v); }

// Drains one endpoint's outbox through `send`, never holding `m` across a send. At most one caller drains
// an outbox at a time (`draining`), which is what keeps frames in order. Returns true when the transport
// reported the endpoint gone (the caller ends the session).
template <class Table, class Id, class SendFn>
bool DrainOutbox(std::mutex& m, Table& table, Id id, SendFn send) {
  std::unique_lock<std::mutex> lock(m);
  auto it = table.find(id);
  if (it == table.end()) return false;
  if (it->second.out.draining || it->second.out.blocked) return false;
  it->second.out.draining = true;
  for (;;) {
    it = table.find(id);
    if (it == table.end()) return false;  // the record was torn down while we were sending
    auto& out = it->second.out;
    if (out.queue.empty()) {
      out.draining = false;
      return false;
    }
    const auto item = out.queue.front();  // shared_ptr copy: the frame outlives a concurrent teardown
    lock.unlock();
    const SendResult result = send(*item.data, item.binary);
    lock.lock();
    it = table.find(id);
    if (it == table.end()) return false;
    auto& after = it->second.out;
    if (result == SendResult::Sent) {
      after.bytes -= item.data->size();
      after.queue.pop_front();
      continue;
    }
    after.draining = false;
    if (result == SendResult::WouldBlock) {
      after.blocked = true;  // OnXWritable resumes; the frame stays at the front
      return false;
    }
    after.queue.clear();
    after.bytes = 0;
    return true;
  }
}

}  // namespace

const char* RoleName(Role role) {
  switch (role) {
    case Role::Config: return "config";
    case Role::Login: return "login";
    case Role::Matchmaker: return "matchmaker";
  }
  return "unknown";
}

Role ClassifyFirstFrame(uint64_t symbol) {
  switch (symbol) {
    case nevr_evr_codec::kSymConfigRequest:
      return Role::Config;
    case nevr_evr_codec::kSymMatchmakerStatusRequest:
    case nevr_evr_codec::kSymFindSessionRequest:
    case nevr_evr_codec::kSymCreateSessionRequest:
    case nevr_evr_codec::kSymJoinSessionRequest:
    case nevr_evr_codec::kSymDirectoryRequest:
    case nevr_evr_codec::kSymPendingSessionCancel:
    case nevr_evr_codec::kSymPlayerSessionsRequest:
    case nevr_evr_codec::kSymLobbyPingResponse:
      return Role::Matchmaker;
    default:
      return Role::Login;  // LogInRequestv2, or anything the login connection could be sending
  }
}

bool IsLoginSessionReply(uint64_t symbol) {
  switch (symbol) {
    case nevr_evr_codec::kSymLoginSuccess:
    case nevr_evr_codec::kSymLoginFailure:
    case nevr_evr_codec::kSymLoginSettings:
    case nevr_evr_codec::kSymLoggedInUserProfileSuccess:
    case nevr_evr_codec::kSymLoggedInUserProfileFailure:
    case nevr_evr_codec::kSymDocumentSuccess:
    case nevr_evr_codec::kSymDocumentFailure:
    case nevr_evr_codec::kSymOtherUserProfileSuccess:
    case nevr_evr_codec::kSymOtherUserProfileFailure:
    case nevr_evr_codec::kSymUpdateProfileSuccess:
    case nevr_evr_codec::kSymUpdateProfileFailure:
    case nevr_evr_codec::kSymServerProfileUpdateSuccess:
    case nevr_evr_codec::kSymServerProfileUpdateFailure:
    case nevr_evr_codec::kSymChannelInfoResponse:  // accepted on any peer by the game, but its Unrequire lands where the count is
      return true;
    default:
      return false;
  }
}

bool RequestRaisesRequireCount(Role role, uint64_t symbol) {
  switch (role) {
    case Role::Login:
      // Every login-connection send carries the flag (the login, the profile, channel info, document, update,
      // generic message, match ended, leaderboard, the friends and party sends) except these three.
      return symbol != nevr_evr_codec::kSymLogOut && symbol != nevr_evr_codec::kSymTelemetryEvent &&
             symbol != nevr_evr_codec::kSymRemoteLogSet;
    case Role::Matchmaker:
      return symbol != nevr_evr_codec::kSymMatchmakerStatusRequest;  // the lobby requests and the PingResponse
    case Role::Config:
      return symbol == nevr_evr_codec::kSymConfigRequest;
  }
  return false;
}

// The replies the service follows with an STcpConnectionUnrequireEvent of its own frame (a successful login
// reply carries its Unrequire inside the same frame; a config reply's comes on the config remote). A login
// failure is sent as SendEvrUnrequire: the LoginFailure frame, then a standalone Unrequire.
bool PairsWithUnrequire(uint64_t symbol) {
  return symbol == nevr_evr_codec::kSymLoginFailure || symbol == nevr_evr_codec::kSymChannelInfoResponse || symbol == nevr_evr_codec::kSymDocumentSuccess ||
         symbol == nevr_evr_codec::kSymUpdateProfileSuccess || symbol == nevr_evr_codec::kSymLobbyPingRequest;
}

namespace {

constexpr uint32_t kMaxRequireCount = 1000;  // a connection whose Unrequires never come cannot count for ever

// Calls fn(symbol, index) for every message in `frame` (a WebSocket message can carry several, back to back).
template <class Fn>
void ForEachMessage(const std::string& frame, Fn fn) {
  std::size_t offset = 0;
  for (std::size_t index = 0;; ++index) {
    nevr_evr_codec::Message message;
    if (nevr_evr_codec::ReadMessage(frame, offset, &message) != nevr_evr_codec::ReadStatus::Ok) return;
    fn(message.symbol, index);
    offset += nevr_evr_codec::kHeaderSize + static_cast<std::size_t>(message.length);
  }
}

}  // namespace

// Work recorded under the lock and executed after it is released.
struct Router::Effects {
  struct GameClose {
    GameId game;
    uint16_t code;
    std::string reason;
  };
  std::vector<std::pair<LogLevel, std::string>> logs;
  std::vector<RemoteOpenRequest> opens;
  std::vector<RemoteId> drainRemotes;
  std::vector<GameId> drainGames;
  std::vector<std::pair<RemoteId, uint16_t>> remoteCloses;
  std::vector<GameClose> gameCloses;
  std::vector<std::pair<GameId, bool>> holds;  // SetIdleExempt calls
};

Router::Router(GameTransport* games, RemoteTransport* remotes, Options options)
    : games_(games), remotes_(remotes), options_(std::move(options)) {}

Router::~Router() {
  std::lock_guard<std::mutex> lock(mutex_);
  ForgetLoginFrameLocked();
}

namespace {
// Overwrites a string's bytes through a volatile pointer (the compiler may not drop the stores) and releases it.
void SecureWipe(std::string& s) {
  volatile char* p = s.empty() ? nullptr : &s[0];
  for (std::size_t i = 0; i < s.size(); ++i) p[i] = 0;
  s.clear();
  s.shrink_to_fit();
}
}  // namespace

// The cached LoginRequest carries the player's token: wiped on every release, held in memory only.
void Router::ForgetLoginFrameLocked() {
  SecureWipe(lastLoginFrame_);
  replayPending_ = false;
}

void Router::Log(Effects& fx, LogLevel level, std::string line) {
  if (options_.log) fx.logs.emplace_back(level, std::move(line));
}

void Router::Run(Effects& fx) {
  for (const auto& entry : fx.logs) options_.log(entry.first, entry.second);
  for (const auto& hold : fx.holds) games_->SetIdleExempt(hold.first, hold.second);
  for (const RemoteId remote : fx.drainRemotes) DrainRemote(remote);
  for (const GameId game : fx.drainGames) DrainGame(game);
  for (const RemoteOpenRequest& request : fx.opens) {
    if (!remotes_->Open(request)) FailSession(request.remote, kCloseInternalError, "remote could not be started", false);
  }
  for (const auto& close : fx.remoteCloses) remotes_->Close(close.first, close.second);
  for (const auto& close : fx.gameCloses) games_->Close(close.game, close.code, close.reason);
}

void Router::DrainRemote(RemoteId remote) {
  const bool gone = DrainOutbox(mutex_, remoteTable_, remote, [this, remote](const std::string& data, bool binary) {
    return remotes_->Send(remote, data, binary);
  });
  if (gone) FailSession(remote, kCloseGoingAway, "send to the remote failed", true);
}

void Router::DrainGame(GameId game) {
  const bool gone = DrainOutbox(mutex_, gameTable_, game, [this, game](const std::string& data, bool binary) {
    return games_->Send(game, data, binary);
  });
  if (!gone) return;
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseGameLocked(game, kCloseGoingAway, "send to the game failed", fx);
  }
  Run(fx);
}

void Router::FailSession(RemoteId remote, uint16_t code, const char* why, bool closeRemote) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    FailSessionLocked(remote, code, why, closeRemote, fx);
  }
  Run(fx);
}

void Router::CloseGameLocked(GameId game, uint16_t code, const char* why, Effects& fx) {
  const auto it = gameTable_.find(game);
  if (it == gameTable_.end() || it->second.closing) return;
  it->second.closing = true;
  fx.gameCloses.push_back({game, code, why});
  Log(fx, LogLevel::Warning,
      Fmt("[router] closing game=%llu conn=%d (%s) code=%u: %s", Ull(game), it->second.connIdx,
          RoleName(it->second.role), static_cast<unsigned>(code), why));
}

// A remote session that ends takes the game's sockets on it down too, or the game sits "logged in" with
// no server and never reconnects (#70). Also forgets the session: the game's next connection is a new
// login, not a matchmaker on a dead session.
void Router::FailSessionLocked(RemoteId remote, uint16_t code, const char* why, bool closeRemote, Effects& fx) {
  const auto rit = remoteTable_.find(remote);
  if (rit == remoteTable_.end()) return;
  const int ownerConn = rit->second.ownerConn;
  std::vector<GameId> bound;
  for (const auto& entry : gameTable_) {
    if (entry.second.remote == remote) bound.push_back(entry.first);
  }
  std::sort(bound.begin(), bound.end());
  const bool wasLogin = (remote == loginRemote_);
  // The silent case: the login connection had nothing outstanding, so the game raises no Lost event and
  // reconnects without logging in. With requests outstanding it is the Lost path (-95): the player's RETRY
  // sends the game's own login, and a replay would only put a stale login ahead of it.
  bool outstanding = false;
  if (wasLogin) {
    const auto lit = gameTable_.find(loginGame_);
    outstanding = lit != gameTable_.end() && lit->second.required != 0;
  }
  if (wasLogin) {
    loginRemote_ = kNoRemote;
    loginGame_ = kNoGame;
    activeGame_ = kNoGame;
    owedUnrequires_.clear();
    connectionCount_ = 1;  // the game's next connection is a login
  }
  const bool replayDue =
      wasLogin && options_.replayLoginOnReconnect && !lastLoginFrame_.empty() && !outstanding;
  if (wasLogin) replayPending_ = replayDue;
  Log(fx, LogLevel::Warning,
      Fmt("[router] remote session ended remote=%llu owner_conn=%d code=%u (%s): closing %zu game socket(s)%s%s",
          Ull(remote), ownerConn, static_cast<unsigned>(code), why, bound.size(),
          wasLogin ? "; login session forgotten, next connection is a new login" : "",
          replayDue ? "; the game's last LoginRequest is kept for replay" : ""));
  remoteTable_.erase(rit);
  for (const GameId game : bound) {
    Game& g = gameTable_[game];
    g.remote = kNoRemote;
    g.out.queue.clear();
    g.out.bytes = 0;
    CloseGameLocked(game, code, why, fx);
  }
  if (closeRemote) fx.remoteCloses.emplace_back(remote, code);
}

std::size_t Router::LiveMatchmakersLocked() const {
  std::size_t live = 0;
  for (const auto& entry : gameTable_) {
    if (entry.second.role == Role::Matchmaker && !entry.second.closing) ++live;
  }
  return live;
}

// True when `id` is a live (not closing) connection on the shared login session.
bool Router::OnLoginSessionLocked(GameId id) const {
  if (id == kNoGame || loginRemote_ == kNoRemote) return false;
  const auto it = gameTable_.find(id);
  return it != gameTable_.end() && !it->second.closing && it->second.remote == loginRemote_;
}

// The newest live matchmaker connection on the login session becomes the lobby-traffic target.
void Router::RecomputeActiveLocked() {
  activeGame_ = kNoGame;
  int newest = -1;
  for (const auto& entry : gameTable_) {
    if (entry.second.role == Role::Matchmaker && OnLoginSessionLocked(entry.first) && entry.second.connIdx > newest) {
      newest = entry.second.connIdx;
      activeGame_ = entry.first;
    }
  }
}

// Counts the requests in a game frame that raise the connection's outstanding-request count, as the game
// does. The count is the game's own: its 8-bit count wraps when an Unrequire arrives with none outstanding.
void Router::CountRequirementsLocked(Game& g, const std::string& frame) {
  if (g.remote == kNoRemote) return;
  const Role role = g.role;
  ForEachMessage(frame, [&g, role](uint64_t symbol, std::size_t) {
    if (RequestRaisesRequireCount(role, symbol) && g.required < kMaxRequireCount) ++g.required;
  });
}

// Lowers `target`'s outstanding-request count for one Unrequire delivered to it. False, and nothing lowered,
// when the connection is gone or has nothing outstanding: delivering it would wrap the game's count.
bool Router::TakeUnrequireLocked(GameId target) {
  if (target == kNoGame) return false;
  const auto it = gameTable_.find(target);
  if (it == gameTable_.end() || it->second.closing || it->second.required == 0) return false;
  --it->second.required;
  return true;
}

// Picks the game connection a frame from the login session's remote is delivered to, by what the frame is
// (its first message; Unrequires later in the same frame belong to the messages before them in it):
//   * an answer to a login-connection request (including ChannelInfoResponse): the login connection
//     (libpnsovr drops the login family on any other peer), and nowhere else if that connection is gone;
//   * LobbyPingRequest, the ping the service starts itself: the matchmaker connection, and dropped, with its
//     Unrequire, when there is none;
//   * STcpConnectionUnrequireEvent: the connection that owes it (below), if that connection still has a
//     request outstanding; otherwise dropped, because delivering it would wrap the game's count;
//   * anything else (lobby traffic): the newest matchmaker connection, else the login connection.
// The messages the service pairs with an Unrequire queue the connection they went to; the Unrequires that
// arrive standalone, in the order the service sends them, take the queue's front. Frames from concurrent
// service goroutines interleave, so the queue holds a connection per paired message rather than guessing from
// the message just before.
// *quietDrop is set when the frame is dropped on purpose and counted (an Unrequire, a ping with no matchmaker).
GameId Router::RouteLoginSessionFrameLocked(const std::string& frame, bool* quietDrop) {
  *quietDrop = false;
  const uint64_t symbol = nevr_evr_codec::FirstSymbol(frame);
  std::size_t embedded = 0;
  ForEachMessage(frame, [&embedded](uint64_t s, std::size_t index) {
    if (index > 0 && s == nevr_evr_codec::kSymConnectionUnrequire) ++embedded;
  });
  if (symbol == nevr_evr_codec::kSymConnectionUnrequire) {
    GameId owner = kNoGame;
    if (!owedUnrequires_.empty()) {
      owner = owedUnrequires_.front();
      owedUnrequires_.pop_front();
    }
    if (OnLoginSessionLocked(owner) && TakeUnrequireLocked(owner)) return owner;
    ++droppedUnrequires_;
    *quietDrop = true;
    return kNoGame;
  }
  const bool matchmakerLive = OnLoginSessionLocked(activeGame_) && gameTable_.at(activeGame_).role == Role::Matchmaker;
  GameId target = kNoGame;
  if (symbol == nevr_evr_codec::kSymLobbyPingRequest) {
    if (matchmakerLive) target = activeGame_;
  } else if (IsLoginSessionReply(symbol)) {
    target = OnLoginSessionLocked(loginGame_) ? loginGame_ : kNoGame;
  } else if (matchmakerLive) {
    target = activeGame_;
  } else if (OnLoginSessionLocked(loginGame_)) {
    target = loginGame_;
  }
  if (embedded > 0) {
    // The Unrequires inside the frame belong to its own messages: they lower the count of the connection the
    // frame goes to, never below zero.
    // One the count cannot cover (the connection has nothing outstanding, or the frame is dropped) cannot be
    // stripped out of the frame either: it is delivered with the frame (or dropped with it) and counted.
    std::size_t matched = 0;
    while (matched < embedded && target != kNoGame && TakeUnrequireLocked(target)) ++matched;
    unmatchedEmbeddedUnrequires_ += embedded - matched;
  } else if (PairsWithUnrequire(symbol)) {
    owedUnrequires_.push_back(target);  // kNoGame when the message is dropped: its Unrequire is too
    while (owedUnrequires_.size() > 64) owedUnrequires_.pop_front();
  }
  if (target == kNoGame && symbol == nevr_evr_codec::kSymLobbyPingRequest) {
    ++droppedRemoteFrames_;
    *quietDrop = true;
  }
  return target;
}

bool Router::PushToRemoteLocked(RemoteId remote, Remote& r, std::shared_ptr<const std::string> data, bool binary,
                                Effects& fx) {
  if (r.out.bytes + data->size() > options_.limits.maxOutboundBytes) {
    FailSessionLocked(remote, kCloseTryAgainLater, "outbound buffer to the remote is full", true, fx);
    return false;
  }
  r.out.bytes += data->size();
  r.out.queue.push_back({std::move(data), binary});
  fx.drainRemotes.push_back(remote);
  return true;
}

// Marks the remote open and moves [login request, then every frame the game queued] into its outbox, in
// that order. The login frame is first so the server never sees a game frame on an unauthenticated session.
void Router::FlushOpenLocked(RemoteId remote, Remote& r, std::optional<std::string> login, Effects& fx) {
  r.open = true;
  if (login.has_value()) {
    if (!PushToRemoteLocked(remote, r, std::make_shared<const std::string>(std::move(*login)), true, fx)) return;
  }
  std::deque<Item> pending;
  pending.swap(r.pending);
  r.pendingBytes = 0;
  for (auto& item : pending) {
    if (!PushToRemoteLocked(remote, r, std::move(item.data), item.binary, fx)) return;
  }
}

// ---- game side ---------------------------------------------------------------------------------

// The role a connection that has sent nothing yet is given: by connection order, except that a connection
// opened while the login session has no live login connection is that connection (the game reconnected it).
Role Router::ProvisionalRoleLocked() const {
  if (connectionCount_ == 0) return Role::Config;
  if (connectionCount_ == 1) return Role::Login;
  if (loginRemote_ != kNoRemote && remoteTable_.count(loginRemote_) != 0 && !OnLoginSessionLocked(loginGame_)) {
    return Role::Login;
  }
  return Role::Matchmaker;
}

// Gives `g` (whose connIdx and role are set) its remote: matchmaker connections share the login session
// when one is live; every other connection, and a matchmaker with no session to share, opens its own.
void Router::AttachLocked(GameId game, Game& g, Effects& fx) {
  const bool sessionLive = loginRemote_ != kNoRemote && remoteTable_.count(loginRemote_) != 0;
  if (sessionLive && g.role == Role::Matchmaker) {
    g.remote = loginRemote_;
    activeGame_ = game;
    Log(fx, LogLevel::Info,
        Fmt("[router] game=%llu conn=%d (matchmaker) shares login session remote=%llu (no LoginRequest)", Ull(game),
            g.connIdx, Ull(loginRemote_)));
    return;
  }
  if (sessionLive && g.role == Role::Login) {
    // A login connection on a session that is already live (the game reconnected its login connection, or a
    // connection the router took for a matchmaker turned out to be the login): it takes over the session's
    // login role; the previous login connection, if still open, rides the session as a matchmaker.
    const GameId previous = loginGame_;
    if (previous != kNoGame && previous != game) {
      const auto pit = gameTable_.find(previous);
      if (pit != gameTable_.end()) {
        pit->second.role = Role::Matchmaker;
        fx.holds.emplace_back(previous, false);
      }
    }
    g.remote = loginRemote_;
    loginGame_ = game;
    const bool held = remoteTable_.at(loginRemote_).deferred;
    fx.holds.emplace_back(game, true);
    Log(fx, LogLevel::Info,
        Fmt("[router] game=%llu conn=%d (login) takes over login session remote=%llu%s", Ull(game), g.connIdx,
            Ull(loginRemote_), held ? " (held: waiting for the account)" : ""));
    return;
  }
  const RemoteId remote = nextRemote_++;
  Remote r;
  r.ownerConn = g.connIdx;
  r.request.remote = remote;
  r.request.connIdx = g.connIdx;
  r.request.role = g.role;
  r.request.standaloneMatchmaker = (g.role == Role::Matchmaker);
  // Only the login connection is held: it is silent until the game has an account to log in with, and a
  // config or matchmaker connection with no account must fail at once (the game reconnects those).
  const bool hold = g.role == Role::Login && GateAwaitingLocked();
  r.deferred = hold;
  const RemoteOpenRequest request = r.request;
  remoteTable_.emplace(remote, std::move(r));
  g.remote = remote;
  if (g.role == Role::Login) {
    loginRemote_ = remote;
    loginGame_ = game;
  }
  if (g.role == Role::Login) fx.holds.emplace_back(game, true);
  if (hold) {
    Log(fx, LogLevel::Info,
        Fmt("[router] game=%llu conn=%d (login) held: remote=%llu opens when the account is available", Ull(game),
            g.connIdx, Ull(remote)));
    return;
  }
  fx.opens.push_back(request);
  Log(fx, LogLevel::Info,
      Fmt("[router] game=%llu conn=%d (%s) opened remote=%llu", Ull(game), g.connIdx, RoleName(g.role), Ull(remote)));
}

bool Router::GateAwaitingLocked() const {
  return options_.loginGate && options_.loginGate() == LoginGate::Awaiting;
}

// Lets go of the remote `g` holds so it can be given another. A matchmaker only shares the login session; a
// config connection's remote is its own and is closed; a login connection that is alone on its session takes
// the session with it (the next connection is a login again). False when the connection is the login
// connection of a session other connections ride: that session is not ours to end here.
bool Router::ReleaseRemoteLocked(GameId game, Game& g, Effects& fx) {
  if (g.remote == kNoRemote) return true;
  if (g.remote == loginRemote_) {
    if (g.role == Role::Matchmaker) {
      g.remote = kNoRemote;
      return true;
    }
    for (const auto& entry : gameTable_) {
      if (entry.first != game && !entry.second.closing && entry.second.remote == loginRemote_) return false;
    }
    Log(fx, LogLevel::Info,
        Fmt("[router] login session remote=%llu ended with its only connection game=%llu; next connection is a login",
            Ull(loginRemote_), Ull(game)));
    loginRemote_ = kNoRemote;
    loginGame_ = kNoGame;
    owedUnrequires_.clear();
    connectionCount_ = 1;
    // The session the game's login was on is gone with its only connection (a reconnect that turned out not
    // to be the login socket, or a close): the game's next login socket reconnects silently too.
    if (options_.replayLoginOnReconnect && !lastLoginFrame_.empty()) replayPending_ = true;
  }
  g.required = 0;  // the requests it sent went with the remote it leaves
  remoteTable_.erase(g.remote);
  fx.remoteCloses.emplace_back(g.remote, static_cast<uint16_t>(1000));
  g.remote = kNoRemote;
  return true;
}

// The first data frame of a connection names its role. A frame that contradicts the provisional role (set by
// connection order when the socket opened) moves the connection: it is detached from the remote it was given
// and attached to the one its real role uses. Unknown symbols name the login role (ClassifyFirstFrame), but
// they never take a connection that is not already the login connection: only LogInRequestv2 does, so an
// unlisted lobby message cannot steal the login session's replies.
void Router::ClassifyGameLocked(GameId game, Game& g, uint64_t symbol, Effects& fx) {
  g.classified = true;
  Role observed = ClassifyFirstFrame(symbol);
  if (observed == Role::Login && g.role != Role::Login && symbol != nevr_evr_codec::kSymLoginRequest) observed = g.role;
  if (observed == g.role) return;
  const Role provisional = g.role;
  if (!ReleaseRemoteLocked(game, g, fx)) {
    Log(fx, LogLevel::Warning,
        Fmt("[router] game=%llu conn=%d first frame names %s but the connection owns the shared login session; role "
            "stays %s",
            Ull(game), g.connIdx, RoleName(observed), RoleName(provisional)));
    return;
  }
  if (loginGame_ == game) loginGame_ = kNoGame;
  if (provisional == Role::Login) fx.holds.emplace_back(game, false);
  g.role = observed;
  Log(fx, LogLevel::Info,
      Fmt("[router] game=%llu conn=%d first frame symbol=0x%016llx: %s -> %s", Ull(game), g.connIdx, Ull(symbol),
          RoleName(provisional), RoleName(observed)));
  AttachLocked(game, g, fx);
  RecomputeActiveLocked();
}

void Router::OnGameOpen(GameId game) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) {
      fx.gameCloses.push_back({game, kCloseGoingAway, "router shut down"});
    } else if (gameTable_.count(game) != 0) {
      Log(fx, LogLevel::Error, Fmt("[router] duplicate open for game=%llu ignored", Ull(game)));
    } else if (const Role next = ProvisionalRoleLocked();
               next == Role::Matchmaker && LiveMatchmakersLocked() >= options_.limits.maxMatchmakerConnections) {
      Log(fx, LogLevel::Warning,
          Fmt("[router] game=%llu refused: %zu matchmaker connections are already open", Ull(game),
              options_.limits.maxMatchmakerConnections));
      fx.gameCloses.push_back({game, kCloseTryAgainLater, "too many matchmaker connections"});
    } else {
      Game g;
      g.connIdx = connectionCount_++;
      g.role = next;
      AttachLocked(game, g, fx);
      gameTable_.emplace(game, std::move(g));
    }
  }
  Run(fx);
}

void Router::OnGameFrame(GameId game, std::string frame, bool binary) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (options_.replayLoginOnReconnect) {
      // A LogOut anywhere in any game frame ends the account the cache belongs to, whichever connection sent
      // it and whether or not the frame is then dropped.
      bool loggedOut = false;
      ForEachMessage(frame, [&loggedOut](uint64_t s, std::size_t) {
        if (s == nevr_evr_codec::kSymLogOut) loggedOut = true;
      });
      if (loggedOut) ForgetLoginFrameLocked();
    }
    const auto git = gameTable_.find(game);
    if (git == gameTable_.end() || git->second.closing || git->second.remote == kNoRemote) {
      const uint64_t dropped = ++droppedGameFrames_;
      if (dropped == 1 || dropped % 100 == 0) {
        Log(fx, LogLevel::Warning,
            Fmt("[router] game->server frame DROPPED: game=%llu has no live session bytes=%zu (%llu total)",
                Ull(game), frame.size(), Ull(dropped)));
      }
    } else if (frame.size() > options_.limits.maxFrameBytes) {
      Log(fx, LogLevel::Error,
          Fmt("[router] game->server frame too large: game=%llu bytes=%zu limit=%zu", Ull(game), frame.size(),
              options_.limits.maxFrameBytes));
      CloseGameLocked(game, kCloseMessageTooBig, "frame exceeds the size limit", fx);
    } else {
      if (!git->second.classified) ClassifyGameLocked(game, git->second, nevr_evr_codec::FirstSymbol(frame), fx);
      const RemoteId remoteId = git->second.remote;
      const auto rit = remoteTable_.find(remoteId);
      if (rit == remoteTable_.end()) {
        ++droppedGameFrames_;
        CloseGameLocked(game, kCloseGoingAway, "session record missing", fx);
      } else {
        Remote& r = rit->second;
        CountRequirementsLocked(git->second, frame);
        const uint64_t firstSymbol = nevr_evr_codec::FirstSymbol(frame);
        if (options_.replayLoginOnReconnect && r.open && git->second.role == Role::Login && git->second.classified &&
            ReplayDueLocked(remoteId, r)) {
          // The connection has just shown itself to be the login socket. Its own login needs no replay; any
          // other request is sent behind the replay.
          if (firstSymbol == nevr_evr_codec::kSymLoginRequest) {
            replayPending_ = false;
            Log(fx, LogLevel::Info,
                Fmt("[router] login replay skipped remote=%llu conn=%d: the game sent its own login", Ull(remoteId),
                    r.ownerConn));
          } else {
            SendReplayLocked(remoteId, r, fx);
          }
        }
        if (options_.replayLoginOnReconnect && git->second.role == Role::Login &&
            firstSymbol == nevr_evr_codec::kSymLoginRequest) {
          SecureWipe(lastLoginFrame_);
          lastLoginFrame_ = frame;  // a refreshed token arrives as a new LoginRequest and replaces it
        }
        auto data = std::make_shared<const std::string>(std::move(frame));
        if (r.open) {
          PushToRemoteLocked(remoteId, r, std::move(data), binary, fx);
        } else if (r.pending.size() >= options_.limits.maxPendingFrames ||
                   r.pendingBytes + data->size() > options_.limits.maxPendingBytes) {
          Log(fx, LogLevel::Warning,
              Fmt("[router] game->server queue full while remote=%llu is not open: frames=%zu bytes=%zu",
                  Ull(remoteId), r.pending.size(), r.pendingBytes));
          CloseGameLocked(game, kCloseTryAgainLater, "pending queue is full", fx);
        } else {
          r.pendingBytes += data->size();
          r.pending.push_back({std::move(data), binary});
        }
      }
    }
  }
  Run(fx);
}

void Router::OnGameClose(GameId game) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = gameTable_.find(game);
    if (it == gameTable_.end()) return;
    const int connIdx = it->second.connIdx;
    const Role role = it->second.role;
    const RemoteId remote = it->second.remote;
    gameTable_.erase(it);
    if (loginGame_ == game) loginGame_ = kNoGame;
    if (activeGame_ == game) {
      // Lobby traffic goes to the newest matchmaker connection still on the session.
      RecomputeActiveLocked();
      GameId target = activeGame_ != kNoGame ? activeGame_ : (OnLoginSessionLocked(loginGame_) ? loginGame_ : kNoGame);
      Log(fx, LogLevel::Info,
          Fmt("[router] server->game routing: game=%llu closed, lobby frames now go to game=%llu", Ull(game),
              Ull(target)));
    }
    // A remote that only this game used ends with it. The login session is shared with the matchmaker
    // connections and stays until the session itself ends.
    if (remote != kNoRemote && remote != loginRemote_) {
      remoteTable_.erase(remote);
      fx.remoteCloses.emplace_back(remote, static_cast<uint16_t>(1000));
    }
    Log(fx, LogLevel::Info, Fmt("[router] game=%llu disconnected conn=%d (%s)", Ull(game), connIdx, RoleName(role)));
  }
  Run(fx);
}

bool Router::ReplayDueLocked(RemoteId remote, const Remote& r) const {
  return options_.replayLoginOnReconnect && replayPending_ && remote == loginRemote_ && remote != kNoRemote &&
         r.request.role == Role::Login && !r.loginSent && !lastLoginFrame_.empty();
}

GameId Router::LoginConnectionLocked(RemoteId remote) const {
  GameId found = kNoGame;
  for (const auto& entry : gameTable_) {
    if (entry.second.remote == remote && entry.second.role == Role::Login && !entry.second.closing) {
      if (found == kNoGame || entry.first < found) found = entry.first;
    }
  }
  return found;
}

void Router::SendReplayLocked(RemoteId remote, Remote& r, Effects& fx) {
  replayPending_ = false;
  r.loginSent = true;
  ++loginsReplayed_;
  Log(fx, LogLevel::Info,
      Fmt("[router] login replayed remote=%llu conn=%d size=%zu (the game reconnected without a login)", Ull(remote),
          r.ownerConn, lastLoginFrame_.size()));
  PushToRemoteLocked(remote, r, std::make_shared<const std::string>(lastLoginFrame_), true, fx);
}

void Router::OnGameSilent(GameId game) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto git = gameTable_.find(game);
    if (git == gameTable_.end() || git->second.closing || git->second.classified || git->second.silent) return;
    git->second.silent = true;
    const auto rit = remoteTable_.find(git->second.remote);
    if (rit != remoteTable_.end() && rit->second.open && git->second.role == Role::Login &&
        ReplayDueLocked(git->second.remote, rit->second)) {
      SendReplayLocked(git->second.remote, rit->second, fx);
    }
  }
  Run(fx);
}

void Router::OnGameWritable(GameId game) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = gameTable_.find(game);
    if (it == gameTable_.end()) return;
    it->second.out.blocked = false;
  }
  DrainGame(game);
}

// ---- remote side -------------------------------------------------------------------------------

void Router::OnRemoteOpen(RemoteId remote) {
  bool needLogin = false;
  {
    Effects fx;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = remoteTable_.find(remote);
      if (it == remoteTable_.end() || it->second.open || it->second.loginPending) return;
      Remote& r = it->second;
      needLogin = (r.ownerConn == 1 && !r.loginSent && static_cast<bool>(options_.buildLogin));
      if (needLogin) {
        // Frames the game sends from here until the login is queued stay in `pending`, behind it.
        r.loginPending = true;
      } else {
        std::optional<std::string> replay;
        if (ReplayDueLocked(remote, r)) {
          const GameId lg = LoginConnectionLocked(remote);
          const auto lit = gameTable_.find(lg);
          const bool gameLoginFirst =
              !r.pending.empty() && nevr_evr_codec::FirstSymbol(*r.pending.front().data) == nevr_evr_codec::kSymLoginRequest;
          const bool knownLogin = lit != gameTable_.end() &&
                                  (lit->second.silent || (lit->second.classified && lit->second.role == Role::Login));
          if (gameLoginFirst) {
            replayPending_ = false;
            Log(fx, LogLevel::Info,
                Fmt("[router] login replay skipped remote=%llu conn=%d: the game sent its own login first", Ull(remote),
                    r.ownerConn));
          } else if (knownLogin) {
            replay = lastLoginFrame_;
            replayPending_ = false;
            r.loginSent = true;
            ++loginsReplayed_;
            Log(fx, LogLevel::Info,
                Fmt("[router] login replayed remote=%llu conn=%d size=%zu (the game reconnected without a login)",
                    Ull(remote), r.ownerConn, replay->size()));
          } else {
            Log(fx, LogLevel::Info,
                Fmt("[router] login replay held remote=%llu conn=%d: the connection is not yet known to be the "
                    "login socket", Ull(remote), r.ownerConn));
          }
        }
        Log(fx, LogLevel::Info, Fmt("[router] remote=%llu open", Ull(remote)));
        FlushOpenLocked(remote, r, std::move(replay), fx);
      }
    }
    Run(fx);
  }
  if (!needLogin) return;

  // The builder reaches into token storage and game state: never call it under the lock.
  std::optional<std::string> login;
  try {
    login = options_.buildLogin();
  } catch (const std::exception&) {
    login.reset();
  }
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = remoteTable_.find(remote);
    if (it == remoteTable_.end()) return;  // the session ended while the login was being built
    Remote& r = it->second;
    r.loginPending = false;
    if (login.has_value()) {
      r.loginSent = true;
      Log(fx, LogLevel::Info,
          Fmt("[router] login injected remote=%llu conn=%d size=%zu", Ull(remote), r.ownerConn, login->size()));
    } else {
      Log(fx, LogLevel::Error,
          Fmt("[router] login NOT injected remote=%llu conn=%d: no login frame available (identity missing)",
              Ull(remote), r.ownerConn));
    }
    FlushOpenLocked(remote, r, std::move(login), fx);
  }
  Run(fx);
}

void Router::OnRemoteFrame(RemoteId remote, std::string frame, bool binary) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto rit = remoteTable_.find(remote);
    if (rit == remoteTable_.end()) {
      ++droppedRemoteFrames_;
      return;
    }
    if (frame.size() > options_.limits.maxFrameBytes) {
      Log(fx, LogLevel::Error,
          Fmt("[router] server->game frame too large: remote=%llu bytes=%zu limit=%zu", Ull(remote), frame.size(),
              options_.limits.maxFrameBytes));
      FailSessionLocked(remote, kCloseMessageTooBig, "remote frame exceeds the size limit", true, fx);
    } else {
      const uint64_t symbol = nevr_evr_codec::FirstSymbol(frame);
      if (symbol == nevr_evr_codec::kSymLoginFailure) {
        // Numeric diagnostics only: the server's message text can carry private data.
        const std::optional<nevr_evr_codec::LoginFailure> failure = nevr_evr_codec::ParseLoginFailure(frame);
        Log(fx, LogLevel::Warning,
            Fmt("[router] LOGIN FAILURE remote=%llu status=%llu message_bytes=%zu", Ull(remote),
                Ull(failure ? failure->statusCode : 0), failure ? failure->messageBytes : static_cast<std::size_t>(0)));
        if (options_.replayLoginOnReconnect && remote == loginRemote_) ForgetLoginFrameLocked();  // not a token to keep
      } else if (symbol == nevr_evr_codec::kSymLoginSuccess) {
        Log(fx, LogLevel::Info, Fmt("[router] LOGIN SUCCESS remote=%llu", Ull(remote)));
        if (options_.subscribeFriendList && remote == loginRemote_) {
          PushToRemoteLocked(remote, rit->second,
                             std::make_shared<const std::string>(nevr_evr_codec::BuildFriendListSubscribe()), true, fx);
        }
      }
      if (remoteTable_.count(remote) != 0) {
        GameId target = kNoGame;
        bool quietDrop = false;
        if (remote == loginRemote_) {
          target = RouteLoginSessionFrameLocked(frame, &quietDrop);
        } else {
          for (const auto& entry : gameTable_) {
            if (entry.second.remote == remote && !entry.second.closing) target = entry.first;
          }
          // The connection's own remote (config): the same rule, its own count.
          if (target != kNoGame && symbol == nevr_evr_codec::kSymConnectionUnrequire && !TakeUnrequireLocked(target)) {
            ++droppedUnrequires_;
            quietDrop = true;
            target = kNoGame;
          }
        }
        if (target == kNoGame && quietDrop) {
          const uint64_t dropped = droppedUnrequires_ + droppedRemoteFrames_;
          if (dropped == 1 || dropped % 100 == 0) {
            Log(fx, LogLevel::Warning,
                Fmt("[router] server->game frame dropped on purpose: remote=%llu symbol=0x%016llx (%llu total)",
                    Ull(remote), Ull(symbol), Ull(dropped)));
          }
        } else if (target == kNoGame) {
          const uint64_t dropped = ++droppedRemoteFrames_;
          if (dropped == 1 || dropped % 100 == 0) {
            Log(fx, LogLevel::Warning,
                Fmt("[router] server->game frame DROPPED: no game connection is open on remote=%llu bytes=%zu "
                    "(%llu total)", Ull(remote), frame.size(), Ull(dropped)));
          }
        } else {
          Game& g = gameTable_[target];
          if (g.out.bytes + frame.size() > options_.limits.maxOutboundBytes) {
            CloseGameLocked(target, kCloseTryAgainLater, "outbound buffer to the game is full", fx);
          } else {
            g.out.bytes += frame.size();
            g.out.queue.push_back({std::make_shared<const std::string>(std::move(frame)), binary});
            fx.drainGames.push_back(target);
          }
        }
      }
    }
  }
  Run(fx);
}

void Router::OnRemoteClose(RemoteId remote, uint16_t code) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    FailSessionLocked(remote, kCloseGoingAway, Fmt("remote closed, code=%u", static_cast<unsigned>(code)).c_str(),
                      false, fx);
  }
  Run(fx);
}

void Router::OnRemoteError(RemoteId remote, int status, std::string_view what) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string why = Fmt("remote error: http_status=%d %.*s", status, static_cast<int>(what.size()), what.data());
    FailSessionLocked(remote, kCloseGoingAway, why.c_str(), false, fx);
  }
  Run(fx);
}

void Router::OnRemoteWritable(RemoteId remote) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = remoteTable_.find(remote);
    if (it == remoteTable_.end()) return;
    it->second.out.blocked = false;
  }
  DrainRemote(remote);
}

void Router::ReevaluateHeldLogins() {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_ || !options_.loginGate) return;
    const LoginGate gate = options_.loginGate();
    if (gate == LoginGate::Awaiting) return;
    std::vector<RemoteId> held;
    for (const auto& entry : remoteTable_) {
      if (entry.second.deferred) held.push_back(entry.first);
    }
    std::sort(held.begin(), held.end());
    for (const RemoteId id : held) {
      const auto rit = remoteTable_.find(id);
      if (rit == remoteTable_.end()) continue;
      if (gate == LoginGate::Ready) {
        rit->second.deferred = false;
        fx.opens.push_back(rit->second.request);
        Log(fx, LogLevel::Info, Fmt("[router] remote=%llu: the account is available; opening the held login", Ull(id)));
      } else {
        // The sign-in expired or failed: the hold ends with a close, so the game sees a failed connect.
        FailSessionLocked(id, kCloseInternalError, "the account the login needs will not be available", false, fx);
      }
    }
  }
  Run(fx);
}

void Router::Shutdown() {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return;
    shutdown_ = true;
    for (const auto& entry : remoteTable_) fx.remoteCloses.emplace_back(entry.first, kCloseGoingAway);
    for (const auto& entry : gameTable_) {
      if (!entry.second.closing) fx.gameCloses.push_back({entry.first, kCloseGoingAway, "router shut down"});
    }
    remoteTable_.clear();
    gameTable_.clear();
    loginRemote_ = kNoRemote;
    loginGame_ = kNoGame;
    activeGame_ = kNoGame;
    owedUnrequires_.clear();
    ForgetLoginFrameLocked();
    connectionCount_ = 0;
    Log(fx, LogLevel::Info, Fmt("[router] shutdown: %zu remote(s), %zu game(s) closed", fx.remoteCloses.size(),
                                fx.gameCloses.size()));
  }
  Run(fx);
}

Stats Router::GetStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats stats;
  stats.games = gameTable_.size();
  stats.remotes = remoteTable_.size();
  for (const auto& entry : remoteTable_) {
    if (entry.second.deferred) ++stats.heldRemotes;
    stats.pendingFrames += entry.second.pending.size();
    stats.outboundBytes += entry.second.out.bytes;
  }
  for (const auto& entry : gameTable_) stats.outboundBytes += entry.second.out.bytes;
  stats.nextConnIdx = connectionCount_;
  stats.droppedGameFrames = droppedGameFrames_;
  stats.droppedRemoteFrames = droppedRemoteFrames_;
  stats.loginsReplayed = loginsReplayed_;
  stats.loginFrameCached = !lastLoginFrame_.empty();
  stats.loginReplayDue = replayPending_;
  stats.droppedUnrequires = droppedUnrequires_;
  stats.unmatchedEmbeddedUnrequires = unmatchedEmbeddedUnrequires_;
  return stats;
}

}  // namespace nevr_session_router
