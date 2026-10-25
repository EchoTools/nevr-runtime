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

bool IsConfigSessionReply(uint64_t symbol) {
  return symbol == nevr_evr_codec::kSymConfigSuccess || symbol == nevr_evr_codec::kSymConfigFailure;
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
// reply carries its Unrequire inside the same frame). A login failure is sent as SendEvrUnrequire: the
// LoginFailure frame, then a standalone Unrequire; a config reply (success or failure) is followed by a separate
// Unrequire the same way.
bool PairsWithUnrequire(uint64_t symbol) {
  return symbol == nevr_evr_codec::kSymLoginFailure || symbol == nevr_evr_codec::kSymChannelInfoResponse || symbol == nevr_evr_codec::kSymDocumentSuccess ||
         symbol == nevr_evr_codec::kSymUpdateProfileSuccess || symbol == nevr_evr_codec::kSymLobbyPingRequest ||
         symbol == nevr_evr_codec::kSymConfigSuccess || symbol == nevr_evr_codec::kSymConfigFailure;
}

namespace {

constexpr uint32_t kMaxRequireCount = 1000;  // a connection whose Unrequires never come cannot count for ever
constexpr std::size_t kMaxConfigRequests = 256;  // config requests awaiting a reply on one session

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
  std::vector<RemoteId> injections;  // sessions that need the login builder run (no lock held)
  bool sessionEnded = false;         // the shared session's remote ended (Options::onSessionEnded)
};

Router::Router(GameTransport* games, RemoteTransport* remotes, Options options)
    : games_(games), remotes_(remotes), options_(std::move(options)) {}

Router::~Router() = default;

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
  for (const RemoteId remote : fx.injections) BuildAndSendLogin(remote);
  if (fx.sessionEnded && options_.onSessionEnded) options_.onSessionEnded();
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
  PurgePendingLocked(game);
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
  const bool wasSession = (remote == sessionRemote_);
  // The silent case: the login connection had nothing outstanding, so the game raises no Lost event and
  // reconnects without logging in. With requests outstanding it is the Lost path (-95): the player's RETRY
  // sends the game's own login and a notice would only get in front of it.
  // A login socket that closed itself before the failure leaves nothing to reconnect: not armed either.
  bool outstanding = false;
  bool loginSocketGone = false;
  if (wasSession) {
    const auto lit = gameTable_.find(loginGame_);
    loginSocketGone = lit == gameTable_.end() || lit->second.closing;
    outstanding = !loginSocketGone && lit->second.required != 0;
  }
  if (wasSession) {
    fx.sessionEnded = true;
    sessionRemote_ = kNoRemote;
    loginGame_ = kNoGame;
    activeGame_ = kNoGame;
    owedUnrequires_.clear();
    configRequests_.clear();
    connectionCount_ = 1;  // the game's next connection is a login
  }
  const bool removalDue = wasSession && !options_.loginRemovedJson.empty() && haveLoginUser_ && !outstanding &&
                          !loginSocketGone;
  if (wasSession) {
    removalDue_ = removalDue;
  }
  Log(fx, LogLevel::Warning,
      Fmt("[router] remote session ended remote=%llu owner_conn=%d code=%u (%s): closing %llu game socket(s)%s%s",
          Ull(remote), ownerConn, static_cast<unsigned>(code), why, Ull(bound.size()),
          wasSession ? "; login session forgotten, next connection is a new login" : "",
          removalDue ? "; each socket that reconnects is sent one login-removed notice (the game drops it on all but its login peer)" : ""));
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

// True when `id` is a live (not closing) connection on the session remote.
bool Router::OnSessionLocked(GameId id) const {
  if (id == kNoGame || sessionRemote_ == kNoRemote) return false;
  const auto it = gameTable_.find(id);
  return it != gameTable_.end() && !it->second.closing && it->second.remote == sessionRemote_;
}

// The newest live matchmaker connection on the session becomes the lobby-traffic target until one asks.
void Router::RecomputeActiveLocked() {
  const auto current = gameTable_.find(activeGame_);
  if (current != gameTable_.end() && current->second.role == Role::Matchmaker && OnSessionLocked(activeGame_)) return;
  activeGame_ = kNoGame;
  int newest = -1;
  for (const auto& entry : gameTable_) {
    if (entry.second.role == Role::Matchmaker && OnSessionLocked(entry.first) && entry.second.connIdx > newest) {
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

// Picks the game connection a frame from the session's remote is delivered to, by what the frame is
// (its first message; Unrequires later in the same frame belong to the messages before them in it):
//   * an answer to a login-connection request (including ChannelInfoResponse): the login connection
//     (libpnsovr drops the login family on any other peer), and nowhere else if that connection is gone;
//   * SNSConfigSuccessv2 / SNSConfigFailurev2: the connection that sent the OLDEST open config request with the
//     reply's (type, id) (the service echoes the request's two strings, a success as symbols, a failure as the
//     strings), so a request the service never answers does not shift the others' replies. When that request's
//     connection has closed, a live connection's open request for the same key takes the reply (both entries go);
//     with none, or when no open request has that key, the reply is dropped, counted and logged;
//   * LobbyPingRequest, the ping the service starts itself: the matchmaker connection, and dropped, with its
//     Unrequire, when there is none;
//   * STcpConnectionUnrequireEvent: the connection that owes it (below), if that connection still has a
//     request outstanding; otherwise dropped, because delivering it would wrap the game's count;
//   * anything else (lobby traffic): the newest matchmaker connection, else the login connection.
// The messages the service pairs with an Unrequire queue the connection they went to; the Unrequires that
// arrive standalone, in the order the service sends them, take the queue's front. Frames from concurrent
// service goroutines interleave, so the queue holds a connection per paired message rather than guessing from
// the message just before.
// *quietDrop is set when the frame is dropped on purpose and counted (an Unrequire, a ping with no matchmaker,
// a config reply nobody asked for or whose requester closed); *logged is set when the drop has already been
// logged here, so the generic drop line is not written for it.
GameId Router::RouteSessionFrameLocked(const std::string& frame, bool* quietDrop, bool* logged, Effects& fx) {
  *quietDrop = false;
  *logged = false;
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
    if (OnSessionLocked(owner) && TakeUnrequireLocked(owner)) return owner;
    ++droppedUnrequires_;
    *quietDrop = true;
    return kNoGame;
  }
  const bool matchmakerLive = OnSessionLocked(activeGame_) && gameTable_.at(activeGame_).role == Role::Matchmaker;
  GameId target = kNoGame;
  if (symbol == nevr_evr_codec::kSymLobbyPingRequest) {
    if (matchmakerLive) target = activeGame_;
  } else if (IsConfigSessionReply(symbol)) {
    nevr_evr_codec::Message first;
    std::optional<nevr_evr_codec::ConfigKey> key;
    if (nevr_evr_codec::ReadMessage(frame, 0, &first) == nevr_evr_codec::ReadStatus::Ok) {
      key = nevr_evr_codec::ParseConfigReplyKey(
          symbol, std::string_view(reinterpret_cast<const char*>(first.payload), static_cast<std::size_t>(first.length)));
    }
    auto request = configRequests_.begin();
    while (request != configRequests_.end() && !(request->key == key)) ++request;
    if (request == configRequests_.end()) {
      ++configRepliesUnmatched_;
      *quietDrop = true;
      *logged = true;
      std::string open;  // the open requests' keys, so a reply that matches none can be compared with them
      for (std::size_t i = 0; i < configRequests_.size() && i < 4; ++i) {
        const ConfigRequestRecord& entry = configRequests_[i];
        open += Fmt(" game=%llu:%016llx/%016llx", Ull(entry.owner), Ull(entry.key ? entry.key->type : 0),
                    Ull(entry.key ? entry.key->id : 0));
      }
      Log(fx, LogLevel::Warning,
          Fmt("[router] config reply dropped: no open config request has its type and id: symbol=0x%016llx "
              "type=0x%016llx id=0x%016llx (%llu such replies in total); %llu open request(s):%s",
              Ull(symbol), Ull(key ? key->type : 0), Ull(key ? key->id : 0), Ull(configRepliesUnmatched_),
              Ull(configRequests_.size()), open.c_str()));
    } else {
      GameId owner = request->owner;
      // Both entries are erased by position, the later one first. Erasing a deque element that is neither the
      // first nor the last invalidates every iterator into it, so no iterator is used after the first erase.
      const std::ptrdiff_t requestIndex = request - configRequests_.begin();
      std::ptrdiff_t liveIndex = -1;
      if (!OnSessionLocked(owner)) {
        // The oldest request with this key is a closed connection's, perhaps one the service never answered. A
        // reply to a resource goes to a connection that asked for that resource, so a live connection's open
        // request with the same key takes it, and both entries go: the stale one and the one it answered.
        auto live = request + 1;
        while (live != configRequests_.end() && !(live->key == key && OnSessionLocked(live->owner))) ++live;
        if (live != configRequests_.end()) {
          const GameId stale = owner;
          owner = live->owner;
          liveIndex = live - configRequests_.begin();
          Log(fx, LogLevel::Info,
              Fmt("[router] config reply type=0x%016llx id=0x%016llx goes to game=%llu: the oldest open request for it, "
                  "game=%llu, has closed",
                  Ull(key ? key->type : 0), Ull(key ? key->id : 0), Ull(owner), Ull(stale)));
        }
      }
      if (liveIndex >= 0) {
        configRequests_.erase(configRequests_.begin() + std::max(requestIndex, liveIndex));
        configRequests_.erase(configRequests_.begin() + std::min(requestIndex, liveIndex));
      } else {
        configRequests_.erase(configRequests_.begin() + requestIndex);
      }
      if (OnSessionLocked(owner)) {
        target = owner;
      } else {
        ++configRepliesOrphaned_;
        *quietDrop = true;
        *logged = true;
        Log(fx, LogLevel::Warning,
            Fmt("[router] config reply dropped: the connection that asked (game=%llu) has closed and no other asked for "
                "it: symbol=0x%016llx type=0x%016llx id=0x%016llx (%llu such replies in total)",
                Ull(owner), Ull(symbol), Ull(key ? key->type : 0), Ull(key ? key->id : 0), Ull(configRepliesOrphaned_)));
      }
    }
  } else if (IsLoginSessionReply(symbol)) {
    target = OnSessionLocked(loginGame_) ? loginGame_ : kNoGame;
  } else if (matchmakerLive) {
    target = activeGame_;
  } else if (OnSessionLocked(loginGame_)) {
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
                                GameId from, Effects& fx) {
  if (r.out.bytes + data->size() > options_.limits.maxOutboundBytes) {
    FailSessionLocked(remote, kCloseTryAgainLater, "outbound buffer to the remote is full", true, fx);
    return false;
  }
  if (from != kNoGame && remote == sessionRemote_) {
    // The service echoes a request's type and id in its reply: remember whose request this is, and by what key.
    std::size_t offset = 0;
    nevr_evr_codec::Message message;
    while (nevr_evr_codec::ReadMessage(*data, offset, &message) == nevr_evr_codec::ReadStatus::Ok) {
      if (message.symbol == nevr_evr_codec::kSymConfigRequest) {
        const std::string_view requestPayload(reinterpret_cast<const char*>(message.payload),
                                              static_cast<std::size_t>(message.length));
        const std::optional<nevr_evr_codec::ConfigKey> requestKey = nevr_evr_codec::ParseConfigRequestKey(requestPayload);
        if (!requestKey || requestKey->type == 0 || requestKey->id == 0) {
          // A request the reply cannot be matched to by its key. The payload names a resource (never a credential).
          std::string shown;
          for (std::size_t i = 0; i < requestPayload.size() && i < 160; ++i) {
            const char c = requestPayload[i];
            shown += (c >= 0x20 && c < 0x7f) ? c : '.';
          }
          Log(fx, LogLevel::Warning,
              Fmt("[router] game=%llu config request has no usable type and id (%s): bytes=%llu payload=\"%s\"", Ull(from),
                  requestKey ? "empty field" : "unreadable", Ull(requestPayload.size()), shown.c_str()));
        }
        configRequests_.push_back({from, requestKey});
        while (configRequests_.size() > kMaxConfigRequests) {
          ++configRequestsEvicted_;
          Log(fx, LogLevel::Warning,
              Fmt("[router] config request list full (%llu): the oldest unanswered request, game=%llu, is forgotten "
                  "(%llu forgotten in total)",
                  Ull(kMaxConfigRequests), Ull(configRequests_.front().owner), Ull(configRequestsEvicted_)));
          configRequests_.pop_front();
        }
      }
      offset += nevr_evr_codec::kHeaderSize + static_cast<std::size_t>(message.length);
    }
  }
  r.out.bytes += data->size();
  r.out.queue.push_back({std::move(data), binary, kNoGame});
  fx.drainRemotes.push_back(remote);
  return true;
}

// Marks the remote open and moves [login request, then every frame the game queued] into its outbox, in
// that order: the login goes ahead of every frame that waited for it, so the service sees no queued game
// frame (a config connection's excepted, which may already have gone out) before the login.
void Router::FlushOpenLocked(RemoteId remote, Remote& r, std::optional<std::string> login, Effects& fx) {
  r.open = true;
  if (login.has_value()) {
    if (!PushToRemoteLocked(remote, r, std::make_shared<const std::string>(std::move(*login)), true, kNoGame, fx)) return;
  }
  std::deque<Item> pending;
  pending.swap(r.pending);
  r.pendingBytes = 0;
  for (auto& item : pending) {
    if (!PushToRemoteLocked(remote, r, std::move(item.data), item.binary, item.game, fx)) return;
  }
}

// Removes the frames `game` queued before its remote opened (or behind the login), with their bytes. Never
// the outbox: DrainOutbox pops its front after an unlocked send.
void Router::PurgePendingLocked(GameId game) {
  for (auto& entry : remoteTable_) {
    Remote& r = entry.second;
    for (auto it = r.pending.begin(); it != r.pending.end();) {
      if (it->game == game) {
        r.pendingBytes -= it->data->size();
        it = r.pending.erase(it);
      } else {
        ++it;
      }
    }
  }
}

// ---- game side ---------------------------------------------------------------------------------

// The role a connection that has sent nothing yet is given when the adapter has no hint: by connection order,
// except that a connection opened while the session has no live login connection is that connection (the game
// reconnected it). With no hinted connections (a wiring is all hinted or all unhinted) a count of 2 or more
// means a session exists (every end of the session sets the count back to 1), and a matchmaker is given only
// while a login connection is attached to it, so an unhinted connection is on the session and a first frame can
// relabel it in place. Hinted matchmakers that opened first count too and hold standalone remotes, which is why
// a wiring does not mix the two.
Role Router::ProvisionalRoleLocked() const {
  if (connectionCount_ == 0) return Role::Config;
  if (connectionCount_ == 1) return Role::Login;
  if (sessionRemote_ != kNoRemote && remoteTable_.count(sessionRemote_) != 0 && !OnSessionLocked(loginGame_)) {
    return Role::Login;
  }
  return Role::Matchmaker;
}

// Creates a remote for `g` (whose connIdx and role are set). The session remote is the one config and login
// connections share; a matchmaker's remote of its own is never the session. A held session (the gate answers
// Awaiting) is not opened yet when a login connection created it; a session a config connection created opens at
// once (#704, #705), and a standalone matchmaker is never held.
void Router::OpenRemoteLocked(GameId game, Game& g, bool session, Effects& fx) {
  const RemoteId remote = nextRemote_++;
  Remote r;
  r.ownerConn = g.connIdx;
  r.request.remote = remote;
  r.request.connIdx = g.connIdx;
  r.request.role = g.role;
  r.request.standaloneMatchmaker = !session && g.role == Role::Matchmaker;
  r.request.session = session;
  r.loginAttached = session && g.role == Role::Login;
  // Only the login connection waits for the account. A config connection opens its remote at once, so with no
  // account the transport refuses it and the game sees a failed connect (1011), as it did before the session was
  // shared; the login connection then opens (and holds) the session of its own (#704).
  const bool hold = session && g.role == Role::Login && GateAwaitingLocked();
  r.deferred = hold;
  const RemoteOpenRequest request = r.request;
  remoteTable_.emplace(remote, std::move(r));
  g.remote = remote;
  if (session) {
    sessionRemote_ = remote;
    if (g.role == Role::Login) {
      loginGame_ = game;
      fx.holds.emplace_back(game, true);
    }
  }
  if (hold) {
    Log(fx, LogLevel::Info,
        Fmt("[router] game=%llu conn=%d (%s) held: remote=%llu opens when the account is available", Ull(game),
            g.connIdx, RoleName(g.role), Ull(remote)));
    return;
  }
  fx.opens.push_back(request);
  Log(fx, LogLevel::Info,
      Fmt("[router] game=%llu conn=%d (%s) opened %s remote=%llu", Ull(game), g.connIdx, RoleName(g.role),
          session ? "session" : "standalone", Ull(remote)));
}

// Gives `g` its remote. Config and login connections share the session, opening it when there is none. A
// matchmaker shares it once a login connection has been attached to it; before that (or with no session) it
// keeps a remote of its own, so it does not wait behind a login that may never come.
void Router::AttachLocked(GameId game, Game& g, Effects& fx) {
  const auto sit = sessionRemote_ == kNoRemote ? remoteTable_.end() : remoteTable_.find(sessionRemote_);
  const bool sessionLive = sit != remoteTable_.end();
  if (g.role == Role::Matchmaker) {
    if (sessionLive && sit->second.loginAttached) {
      g.remote = sessionRemote_;
      // A new socket takes the lobby traffic only when no live matchmaker is carrying it already.
      if (!OnSessionLocked(activeGame_) || gameTable_.at(activeGame_).role != Role::Matchmaker) activeGame_ = game;
      Log(fx, LogLevel::Info,
          Fmt("[router] game=%llu conn=%d (matchmaker) shares login session remote=%llu (no LoginRequest)", Ull(game),
              g.connIdx, Ull(sessionRemote_)));
      return;
    }
    OpenRemoteLocked(game, g, false, fx);
    return;
  }
  if (!sessionLive) {
    OpenRemoteLocked(game, g, true, fx);
    return;
  }
  g.remote = sessionRemote_;
  if (g.role == Role::Login) {
    TakeOverLoginLocked(game, g, fx);
    return;
  }
  Log(fx, LogLevel::Info,
      Fmt("[router] game=%llu conn=%d (config) joins session remote=%llu%s", Ull(game), g.connIdx, Ull(sessionRemote_),
          sit->second.deferred ? " (held: waiting for the account)" : ""));
}

// Makes `game` the session's login connection: the idle exemption moves to it. The previous login connection,
// if still open, rides the session as a matchmaker when the adapter did not name its role and the matchmaker
// cap has room; a hinted role is final, so a hinted previous login (and one the cap leaves no room for) is
// closed instead of being given another role.
void Router::TakeOverLoginLocked(GameId game, Game& g, Effects& fx) {
  const GameId previous = loginGame_;
  if (previous != kNoGame && previous != game) {
    const auto pit = gameTable_.find(previous);
    if (pit != gameTable_.end()) {
      fx.holds.emplace_back(previous, false);
      if (pit->second.hinted || LiveMatchmakersLocked() >= options_.limits.maxMatchmakerConnections) {
        CloseGameLocked(previous, kCloseGoingAway, "replaced by a newer login connection", fx);
      } else {
        pit->second.role = Role::Matchmaker;
      }
    }
  }
  loginGame_ = game;
  bool held = false;
  const auto rit = remoteTable_.find(g.remote);
  if (rit != remoteTable_.end()) {
    rit->second.loginAttached = true;
    held = rit->second.deferred;
  }
  fx.holds.emplace_back(game, true);
  Log(fx, LogLevel::Info,
      Fmt("[router] game=%llu conn=%d (login) takes over login session remote=%llu%s", Ull(game), g.connIdx,
          Ull(g.remote), held ? " (held: waiting for the account)" : ""));
}

bool Router::LoginAttachedLocked(RemoteId remote) const { return remote == sessionRemote_ && OnSessionLocked(loginGame_); }

// A login is injected when a login connection is attached to an OPEN session that has sent none and is not
// building one. Called when a connection becomes the login connection (it opens as one, or its first frame
// relabels it), so a build that returned no frame is tried again for the next login connection and not for an
// unrelated open. A session that is not open yet is handled by OnRemoteOpen: whichever comes second injects.
void Router::MaybeInjectLocked(Effects& fx) {
  if (!options_.buildLogin || sessionRemote_ == kNoRemote) return;
  const auto rit = remoteTable_.find(sessionRemote_);
  if (rit == remoteTable_.end()) return;
  Remote& r = rit->second;
  if (!r.open || r.loginSent || r.loginPending || !LoginAttachedLocked(sessionRemote_)) return;
  r.loginPending = true;  // frames of the other connections queue behind it from here
  if (GateAwaitingLocked()) {
    DeferLoginLocked(sessionRemote_, r, fx);
    return;
  }
  fx.injections.push_back(sessionRemote_);
}

// The session is open (a config connection opened it and the transport accepted it) but the account is
// still awaited: the login waits, and the other connections' frames queue behind it, until
// ReevaluateHeldLogins sees the gate Ready.
void Router::DeferLoginLocked(RemoteId remote, Remote& r, Effects& fx) {
  r.loginDeferred = true;
  Log(fx, LogLevel::Info,
      Fmt("[router] remote=%llu: login deferred, the account is not available yet", Ull(remote)));
}

bool Router::GateAwaitingLocked() const {
  return options_.loginGate && options_.loginGate() == LoginGate::Awaiting;
}

// The first data frame of a connection names its role. Without a hint, a frame that contradicts the
// provisional role (set by connection order when the socket opened) relabels the connection IN PLACE on the
// session it shares: the idle exemption and the login role move with it, and nothing is released, closed or
// re-attached. With a hint the role is final: the contradiction is logged, once, and not acted on. Unknown
// symbols name the login role (ClassifyFirstFrame), but they never take a connection that is not already the
// login connection: only LogInRequestv2 does, so an unlisted lobby message cannot steal the login session's
// replies.
void Router::ClassifyGameLocked(GameId game, Game& g, uint64_t symbol, Effects& fx) {
  g.classified = true;
  Role observed = ClassifyFirstFrame(symbol);
  if (observed == Role::Login && g.role != Role::Login && symbol != nevr_evr_codec::kSymLoginRequest) observed = g.role;
  if (observed == g.role) return;
  const Role provisional = g.role;
  if (g.hinted) {
    Log(fx, LogLevel::Warning,
        Fmt("[router] game=%llu conn=%d first frame symbol=0x%016llx names %s: it contradicts the hint, role stays %s",
            Ull(game), g.connIdx, Ull(symbol), RoleName(observed), RoleName(provisional)));
    return;
  }
  if (observed == Role::Matchmaker && LiveMatchmakersLocked() >= options_.limits.maxMatchmakerConnections) {
    Log(fx, LogLevel::Warning,
        Fmt("[router] game=%llu refused: %llu matchmaker connections are already open", Ull(game),
            Ull(options_.limits.maxMatchmakerConnections)));
    CloseGameLocked(game, kCloseTryAgainLater, "too many matchmaker connections", fx);
    return;
  }
  if (provisional == Role::Login) {
    if (loginGame_ == game) loginGame_ = kNoGame;
    fx.holds.emplace_back(game, false);
  }
  g.role = observed;
  Log(fx, LogLevel::Info,
      Fmt("[router] game=%llu conn=%d first frame symbol=0x%016llx: %s -> %s (in place)", Ull(game), g.connIdx,
          Ull(symbol), RoleName(provisional), RoleName(observed)));
  if (observed == Role::Login) TakeOverLoginLocked(game, g, fx);
  RecomputeActiveLocked();
  if (observed == Role::Login) MaybeInjectLocked(fx);
}

void Router::OnGameOpen(GameId game, OpenHint hint) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const Role next = hint.hinted ? hint.role : ProvisionalRoleLocked();
    if (shutdown_) {
      fx.gameCloses.push_back({game, kCloseGoingAway, "router shut down"});
    } else if (gameTable_.count(game) != 0) {
      Log(fx, LogLevel::Error, Fmt("[router] duplicate open for game=%llu ignored", Ull(game)));
    } else if (next == Role::Matchmaker && LiveMatchmakersLocked() >= options_.limits.maxMatchmakerConnections) {
      Log(fx, LogLevel::Warning,
          Fmt("[router] game=%llu refused: %llu matchmaker connections are already open", Ull(game),
              Ull(options_.limits.maxMatchmakerConnections)));
      fx.gameCloses.push_back({game, kCloseTryAgainLater, "too many matchmaker connections"});
    } else {
      Game g;
      g.connIdx = connectionCount_++;
      g.role = next;
      g.hinted = hint.hinted;
      AttachLocked(game, g, fx);
      gameTable_.emplace(game, std::move(g));
      RecomputeActiveLocked();
      if (next == Role::Login) MaybeInjectLocked(fx);
    }
  }
  Run(fx);
}

void Router::OnGameFrame(GameId game, std::string frame, bool binary) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!options_.loginRemovedJson.empty()) {
      // A LogOut anywhere in any game frame ends the account the held id belongs to, whichever connection sent
      // it and whether or not the frame is then dropped.
      bool loggedOut = false;
      ForEachMessage(frame, [&loggedOut](uint64_t s, std::size_t) {
        if (s == nevr_evr_codec::kSymLogOut) loggedOut = true;
      });
      if (loggedOut) ForgetLoginUserLocked();
    }
    const auto git = gameTable_.find(game);
    if (git == gameTable_.end() || git->second.closing || git->second.remote == kNoRemote) {
      const uint64_t dropped = ++droppedGameFrames_;
      if (dropped == 1 || dropped % 100 == 0) {
        Log(fx, LogLevel::Warning,
            Fmt("[router] game->server frame DROPPED: game=%llu has no live session bytes=%llu (%llu total)",
                Ull(game), Ull(frame.size()), Ull(dropped)));
      }
    } else if (frame.size() > options_.limits.maxFrameBytes) {
      Log(fx, LogLevel::Error,
          Fmt("[router] game->server frame too large: game=%llu bytes=%llu limit=%llu", Ull(game), Ull(frame.size()),
              Ull(options_.limits.maxFrameBytes)));
      CloseGameLocked(game, kCloseMessageTooBig, "frame exceeds the size limit", fx);
    } else {
      if (!git->second.classified) ClassifyGameLocked(game, git->second, nevr_evr_codec::FirstSymbol(frame), fx);
      if (git->second.closing) {
        ++droppedGameFrames_;  // the classification refused the connection (matchmaker cap)
      } else {
        if (!options_.loginRemovedJson.empty() && removalDue_) {
          // A game that logs in itself needs no notice, on any socket from here on; any other first request on a
          // socket that reconnected would only get a bare Unrequire, so it is sent the notice (once).
          if (nevr_evr_codec::FirstSymbol(frame) == nevr_evr_codec::kSymLoginRequest) {
            removalDue_ = false;
            Log(fx, LogLevel::Info,
                Fmt("[router] login removed notice disarmed game=%llu conn=%d: the game sent its own login", Ull(game),
                    git->second.connIdx));
          } else {
            SendLoginRemovedLocked(game, git->second, fx);
          }
        }
        const RemoteId remoteId = git->second.remote;
        const auto rit = remoteTable_.find(remoteId);
        if (rit == remoteTable_.end()) {
          ++droppedGameFrames_;
          CloseGameLocked(game, kCloseGoingAway, "session record missing", fx);
        } else {
          Remote& r = rit->second;
          CountRequirementsLocked(git->second, frame);
          if (git->second.role == Role::Matchmaker) {
            // Which socket sent each lobby request, so a reply that the game rejects can be tied to the socket
            // that asked (the headset re-find hang has no record of the sender).
            Log(fx, LogLevel::Info,
                Fmt("[router] game=%llu conn=%d (matchmaker) sent symbol=0x%016llx bytes=%llu", Ull(game),
                    git->second.connIdx, Ull(nevr_evr_codec::FirstSymbol(frame)), Ull(frame.size())));
          }
          if (git->second.role == Role::Matchmaker && remoteId == sessionRemote_ && activeGame_ != game) {
            // The game disconnects a matchmaker socket that gets a reply to a request it never sent. Lobby frames
            // carry no request id the router can match, so "the matchmaker that last sent a frame" stands in for
            // "the socket whose request this answers". The limit: with two live matchmakers both mid-request, a
            // reply to the earlier one goes to the later one (pinned by
            // TestLobbyReplyGoesToTheLastAskerWhenTwoAreMidRequest).
            Log(fx, LogLevel::Info,
                Fmt("[router] server->game routing: game=%llu asked, lobby frames now go to it (was game=%llu)",
                    Ull(game), Ull(activeGame_)));
            activeGame_ = game;
          }
          auto data = std::make_shared<const std::string>(std::move(frame));
          // While the login is being built the other connections' frames wait behind it; a config
          // connection's pass.
          const bool behindLogin = r.loginPending && git->second.role != Role::Config;
          if (r.open && !behindLogin) {
            PushToRemoteLocked(remoteId, r, std::move(data), binary, game, fx);
          } else if (r.pending.size() >= options_.limits.maxPendingFrames ||
                     r.pendingBytes + data->size() > options_.limits.maxPendingBytes) {
            Log(fx, LogLevel::Warning,
                Fmt("[router] game->server queue full while remote=%llu is not open: frames=%llu bytes=%llu",
                    Ull(remoteId), Ull(r.pending.size()), Ull(r.pendingBytes)));
            CloseGameLocked(game, kCloseTryAgainLater, "pending queue is full", fx);
          } else {
            r.pendingBytes += data->size();
            r.pending.push_back({std::move(data), binary, game});
          }
        }
      }
    }
  }
  Run(fx);
}

bool Router::SendToSession(std::string frame) {
  Effects fx;
  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto rit = sessionRemote_ == kNoRemote ? remoteTable_.end() : remoteTable_.find(sessionRemote_);
    if (shutdown_ || rit == remoteTable_.end()) {
      Log(fx, LogLevel::Warning, Fmt("[router] runtime frame dropped: no session bytes=%llu", Ull(frame.size())));
    } else {
      Remote& r = rit->second;
      auto data = std::make_shared<const std::string>(std::move(frame));
      if (r.open && !r.loginPending) {
        accepted = PushToRemoteLocked(sessionRemote_, r, std::move(data), true, kNoGame, fx);
      } else if (r.pending.size() >= options_.limits.maxPendingFrames ||
                 r.pendingBytes + data->size() > options_.limits.maxPendingBytes) {
        Log(fx, LogLevel::Warning,
            Fmt("[router] runtime frame dropped: queue full while remote=%llu is not open", Ull(sessionRemote_)));
      } else {
        r.pendingBytes += data->size();
        r.pending.push_back({std::move(data), true, kNoGame});
        accepted = true;
      }
    }
  }
  Run(fx);
  return accepted;
}

std::optional<RemoteId> Router::SessionRemote() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sessionRemote_ == kNoRemote) return std::nullopt;
  return sessionRemote_;
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
    PurgePendingLocked(game);
    if (loginGame_ == game) loginGame_ = kNoGame;
    if (activeGame_ == game) {
      // Lobby traffic goes to the newest matchmaker connection still on the session.
      RecomputeActiveLocked();
      GameId target = activeGame_ != kNoGame ? activeGame_ : (OnSessionLocked(loginGame_) ? loginGame_ : kNoGame);
      Log(fx, LogLevel::Info,
          Fmt("[router] server->game routing: game=%llu closed, lobby frames now go to game=%llu", Ull(game),
              Ull(target)));
    }
    // A standalone matchmaker's remote ends with it. The session is shared and stays until the session itself
    // ends: a connection closing never ends it.
    if (remote != kNoRemote && remote != sessionRemote_) {
      remoteTable_.erase(remote);
      fx.remoteCloses.emplace_back(remote, static_cast<uint16_t>(1000));
    }
    Log(fx, LogLevel::Info, Fmt("[router] game=%llu disconnected conn=%d (%s)", Ull(game), connIdx, RoleName(role)));
  }
  Run(fx);
}

void Router::ForgetLoginUserLocked() {
  haveLoginUser_ = false;
  loginUser_ = nevr_evr_codec::UserId();
  removalDue_ = false;
}

void Router::SendLoginRemovedLocked(GameId game, Game& g, Effects& fx) {
  if (options_.loginRemovedJson.empty() || !removalDue_ || !haveLoginUser_ || g.closing || g.noticeSent) return;
  g.noticeSent = true;
  std::string frame = nevr_evr_codec::BuildLoginRemovedNotify(loginUser_, nevr_evr_codec::kLoginRemovedReasonText,
                                                              options_.loginRemovedJson);
  ++loginsRemoved_;
  Log(fx, LogLevel::Info,
      Fmt("[router] login removed notice sent game=%llu conn=%d size=%llu (the game reconnected without a login: it "
          "shows RETRY and logs in itself)",
          Ull(game), g.connIdx, Ull(frame.size())));
  if (g.out.bytes + frame.size() > options_.limits.maxOutboundBytes) {
    CloseGameLocked(game, kCloseTryAgainLater, "outbound buffer to the game is full", fx);
    return;
  }
  g.out.bytes += frame.size();
  g.out.queue.push_back({std::make_shared<const std::string>(std::move(frame)), true});
  fx.drainGames.push_back(game);
}

void Router::OnGameSilent(GameId game) {
  Effects fx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto git = gameTable_.find(game);
    if (git == gameTable_.end() || git->second.closing || git->second.classified) return;
    SendLoginRemovedLocked(game, git->second, fx);
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
      // The session opening with a login connection attached: the login goes first. With none attached yet
      // the login is injected when one attaches (MaybeInjectLocked).
      needLogin = static_cast<bool>(options_.buildLogin) && !r.loginSent && LoginAttachedLocked(remote);
      if (needLogin && GateAwaitingLocked()) {
        // Open for the connections that need no login (config's pass); the login waits for the gate.
        Log(fx, LogLevel::Info, Fmt("[router] remote=%llu open", Ull(remote)));
        FlushOpenLocked(remote, r, std::nullopt, fx);
        r.loginPending = true;
        DeferLoginLocked(remote, r, fx);
        needLogin = false;
      } else if (needLogin) {
        // Frames the game sends from here until the login is queued stay in `pending`, behind it.
        r.loginPending = true;
      } else {
        Log(fx, LogLevel::Info, Fmt("[router] remote=%llu open", Ull(remote)));
        FlushOpenLocked(remote, r, std::nullopt, fx);
      }
    }
    Run(fx);
  }
  if (needLogin) BuildAndSendLogin(remote);
}

// Runs the builder with no lock held (it reaches into token storage and game state), then queues the login
// ahead of everything that waited for it.
void Router::BuildAndSendLogin(RemoteId remote) {
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
          Fmt("[router] login injected remote=%llu conn=%d size=%llu", Ull(remote), r.ownerConn, Ull(login->size())));
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
          Fmt("[router] server->game frame too large: remote=%llu bytes=%llu limit=%llu", Ull(remote), Ull(frame.size()),
              Ull(options_.limits.maxFrameBytes)));
      FailSessionLocked(remote, kCloseMessageTooBig, "remote frame exceeds the size limit", true, fx);
    } else {
      const uint64_t symbol = nevr_evr_codec::FirstSymbol(frame);
      if (symbol == nevr_evr_codec::kSymLoginFailure) {
        // Numeric diagnostics only: the server's message text can carry private data.
        const std::optional<nevr_evr_codec::LoginFailure> failure = nevr_evr_codec::ParseLoginFailure(frame);
        Log(fx, LogLevel::Warning,
            Fmt("[router] LOGIN FAILURE remote=%llu status=%llu message_bytes=%llu", Ull(remote),
                Ull(failure ? failure->statusCode : 0), Ull(failure ? failure->messageBytes : static_cast<std::size_t>(0))));
        if (!options_.loginRemovedJson.empty() && remote == sessionRemote_) ForgetLoginUserLocked();
        if (options_.replaceLoginFailure) {
          if (std::optional<std::string> replacement = options_.replaceLoginFailure()) {
            // Synthesized, not the service's: logged as such, and no friend-list subscribe follows it.
            Log(fx, LogLevel::Warning,
                Fmt("[router] LOGIN FAILURE remote=%llu replaced by a synthesized LoginSuccess for the game",
                    Ull(remote)));
            frame = std::move(*replacement);
          }
        }
      } else if (symbol == nevr_evr_codec::kSymLoginSuccess) {
        Log(fx, LogLevel::Info, Fmt("[router] LOGIN SUCCESS remote=%llu", Ull(remote)));
        if (!options_.loginRemovedJson.empty() && remote == sessionRemote_) {
          if (const auto user = nevr_evr_codec::ParseLoginSuccessUserId(frame)) {
            loginUser_ = *user;
            haveLoginUser_ = true;
            removalDue_ = false;  // the next session is established
          }
        }
        if (options_.subscribeFriendList && remote == sessionRemote_) {
          PushToRemoteLocked(remote, rit->second,
                             std::make_shared<const std::string>(nevr_evr_codec::BuildFriendListSubscribe()), true, kNoGame, fx);
        }
      }
      if (remoteTable_.count(remote) != 0) {
        GameId target = kNoGame;
        bool quietDrop = false;
        bool alreadyLogged = false;
        if (remote == sessionRemote_) {
          target = RouteSessionFrameLocked(frame, &quietDrop, &alreadyLogged, fx);
        } else {
          for (const auto& entry : gameTable_) {
            if (entry.second.remote == remote && !entry.second.closing) target = entry.first;
          }
          // A standalone matchmaker's remote: the same rule, its own count.
          if (target != kNoGame && symbol == nevr_evr_codec::kSymConnectionUnrequire && !TakeUnrequireLocked(target)) {
            ++droppedUnrequires_;
            quietDrop = true;
            target = kNoGame;
          }
        }
        if (target == kNoGame && quietDrop) {
          const uint64_t dropped = droppedUnrequires_ + droppedRemoteFrames_;
          if (!alreadyLogged && (dropped == 1 || dropped % 100 == 0)) {
            Log(fx, LogLevel::Warning,
                Fmt("[router] server->game frame dropped on purpose: remote=%llu symbol=0x%016llx (%llu total)",
                    Ull(remote), Ull(symbol), Ull(dropped)));
          }
        } else if (target == kNoGame) {
          const uint64_t dropped = ++droppedRemoteFrames_;
          if (dropped == 1 || dropped % 100 == 0) {
            Log(fx, LogLevel::Warning,
                Fmt("[router] server->game frame DROPPED: no game connection is open on remote=%llu bytes=%llu "
                    "(%llu total)", Ull(remote), Ull(frame.size()), Ull(dropped)));
          }
        } else {
          Game& g = gameTable_[target];
          if (g.role == Role::Matchmaker) {
            Log(fx, LogLevel::Info,
                Fmt("[router] reply symbol=0x%016llx bytes=%llu -> game=%llu conn=%d (matchmaker)", Ull(symbol),
                    Ull(frame.size()), Ull(target), g.connIdx));
          }
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
    std::vector<RemoteId> deferredLogins;
    for (const auto& entry : remoteTable_) {
      if (entry.second.loginDeferred) deferredLogins.push_back(entry.first);
    }
    std::sort(deferredLogins.begin(), deferredLogins.end());
    for (const RemoteId id : deferredLogins) {
      const auto rit = remoteTable_.find(id);
      if (rit == remoteTable_.end()) continue;
      rit->second.loginDeferred = false;
      if (gate == LoginGate::Ready) {
        Log(fx, LogLevel::Info, Fmt("[router] remote=%llu: the account is available; injecting the deferred login", Ull(id)));
        fx.injections.push_back(id);
      } else {
        FailSessionLocked(id, kCloseInternalError, "the account the login needs will not be available", false, fx);
      }
    }
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
        Log(fx, LogLevel::Info, Fmt("[router] remote=%llu: the account is available; opening the held session", Ull(id)));
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
    sessionRemote_ = kNoRemote;
    loginGame_ = kNoGame;
    activeGame_ = kNoGame;
    owedUnrequires_.clear();
    configRequests_.clear();
    ForgetLoginUserLocked();
    connectionCount_ = 0;
    Log(fx, LogLevel::Info, Fmt("[router] shutdown: %llu remote(s), %llu game(s) closed", Ull(fx.remoteCloses.size()),
                                Ull(fx.gameCloses.size())));
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
  stats.loginsRemoved = loginsRemoved_;
  stats.loginUserKnown = haveLoginUser_;
  stats.loginRemovedDue = removalDue_;
  stats.droppedUnrequires = droppedUnrequires_;
  stats.unmatchedEmbeddedUnrequires = unmatchedEmbeddedUnrequires_;
  stats.configRepliesOrphaned = configRepliesOrphaned_;
  stats.configRepliesUnmatched = configRepliesUnmatched_;
  stats.configRequestsEvicted = configRequestsEvicted_;
  return stats;
}

}  // namespace nevr_session_router
