// Host test for the shared EVR session router (src/runtime/compat/session_router.{h,cpp}), driven
// through fake game and remote transports. No sockets, no TLS, no game. The same source compiles with
// the NDK (src/quest/CMakeLists.txt) and runs on the host via `just test-quest-router`.
//
// Each test states the failure it exists to catch. The mutation proof for every one of them is recorded
// in the PR body.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "abi/symbol_hash.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/session_router.h"

using namespace nevr_session_router;

namespace {

constexpr uint64_t kSymSomething = 0x1111222233334444ULL;  // an arbitrary game message
constexpr uint64_t kSymLobbySessionSuccess = 0x6d4de3650ee3110fULL;  // SNSLobbySessionSuccessv5
const std::string kSecret = "SECRET-TOKEN-VALUE";

std::string Msg(uint64_t symbol, const std::string& payload = "x") { return nevr_evr_codec::BuildMessage(symbol, payload); }

struct SentFrame {
  uint64_t id;
  std::string data;
  bool binary;
};
struct CloseRecord {
  uint64_t id;
  uint16_t code;
  std::string reason;
};

class FakeGames : public GameTransport {
 public:
  SendResult Send(GameId game, std::string_view frame, bool binary) override {
    std::lock_guard<std::mutex> lock(mutex);
    ++sendCalls;
    if (!script.empty()) {
      const SendResult r = script.front();
      script.erase(script.begin());
      if (r != SendResult::Sent) return r;
    }
    sent.push_back({game, std::string(frame), binary});
    return SendResult::Sent;
  }
  void SetIdleExempt(GameId game, bool exempt) override {
    std::lock_guard<std::mutex> lock(mutex);
    holds.emplace_back(game, exempt);
  }
  std::vector<std::pair<GameId, bool>> Holds() {
    std::lock_guard<std::mutex> lock(mutex);
    return holds;
  }
  void Close(GameId game, uint16_t code, std::string_view reason) override {
    if (onClose) onClose(game);
    std::lock_guard<std::mutex> lock(mutex);
    closes.push_back({game, code, std::string(reason)});
  }
  std::vector<SentFrame> Sent() {
    std::lock_guard<std::mutex> lock(mutex);
    return sent;
  }
  std::vector<CloseRecord> Closes() {
    std::lock_guard<std::mutex> lock(mutex);
    return closes;
  }
  std::mutex mutex;
  std::vector<SentFrame> sent;
  std::vector<CloseRecord> closes;
  std::vector<std::pair<GameId, bool>> holds;
  std::vector<SendResult> script;  // consumed one per Send call; empty means Sent
  int sendCalls = 0;
  std::function<void(GameId)> onClose;
};

class FakeRemotes : public RemoteTransport {
 public:
  bool Open(const RemoteOpenRequest& request) override {
    std::lock_guard<std::mutex> lock(mutex);
    opens.push_back(request);
    return openResult;
  }
  SendResult Send(RemoteId remote, std::string_view frame, bool binary) override {
    std::lock_guard<std::mutex> lock(mutex);
    ++sendCalls;
    if (!script.empty()) {
      const SendResult r = script.front();
      script.erase(script.begin());
      if (r != SendResult::Sent) return r;
    }
    sent.push_back({remote, std::string(frame), binary});
    return SendResult::Sent;
  }
  void Close(RemoteId remote, uint16_t code) override {
    std::lock_guard<std::mutex> lock(mutex);
    closes.push_back({remote, code, ""});
  }
  std::vector<SentFrame> Sent() {
    std::lock_guard<std::mutex> lock(mutex);
    return sent;
  }
  std::vector<CloseRecord> Closes() {
    std::lock_guard<std::mutex> lock(mutex);
    return closes;
  }
  std::vector<RemoteOpenRequest> Opens() {
    std::lock_guard<std::mutex> lock(mutex);
    return opens;
  }
  std::mutex mutex;
  std::vector<RemoteOpenRequest> opens;
  std::vector<SentFrame> sent;
  std::vector<CloseRecord> closes;
  std::vector<SendResult> script;
  bool openResult = true;
  int sendCalls = 0;
};

struct Logs {
  std::mutex mutex;
  std::vector<std::pair<LogLevel, std::string>> lines;
  void Add(LogLevel level, const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    lines.emplace_back(level, line);
  }
  bool Has(const std::string& needle, int level = -1) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& l : lines) {
      if (level >= 0 && static_cast<int>(l.first) != level) continue;
      if (l.second.find(needle) != std::string::npos) return true;
    }
    return false;
  }
};

struct Rig {
  FakeGames games;
  FakeRemotes remotes;
  Logs logs;
  std::unique_ptr<Router> router;
  explicit Rig(Options options = Options()) {
    if (!options.log) options.log = [this](LogLevel level, const std::string& line) { logs.Add(level, line); };
    router = std::make_unique<Router>(&games, &remotes, std::move(options));
  }
};

Options WithLogin(const std::string& frame) {
  Options o;
  o.buildLogin = [frame]() { return std::optional<std::string>(frame); };
  return o;
}

const std::string kLogin = Msg(nevr_evr_codec::kSymLoginRequest, "LOGIN-PAYLOAD-" + kSecret);

// Opens game 1 (config), 2 (login), 3 (matchmaker). Remote ids are read back from the open requests.
void OpenThree(Rig& rig) {
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  rig.router->OnGameOpen(3);
}

// The remote that carries the session: the last one the router marked as the session remote, else the last
// remote opened as a login, else the last remote opened at all. kNoRemote when nothing was opened.
RemoteId SessionRemote(Rig& rig) {
  const auto opens = rig.remotes.Opens();
  for (auto it = opens.rbegin(); it != opens.rend(); ++it) {
    if (it->session) return it->remote;
  }
  for (auto it = opens.rbegin(); it != opens.rend(); ++it) {
    if (it->role == Role::Login) return it->remote;
  }
  return opens.empty() ? kNoRemote : opens.back().remote;
}

// Whether `game` holds the idle exemption now: the last SetIdleExempt for it said so.
bool Exempt(Rig& rig, GameId game) {
  bool exempt = false;
  for (const auto& h : rig.games.Holds()) {
    if (h.first == game) exempt = h.second;
  }
  return exempt;
}

OpenHint Hinted(Role role) {
  OpenHint h;
  h.role = role;
  h.hinted = true;
  return h;
}

int CountUpstream(Rig& rig, RemoteId remote, uint64_t symbol) {
  int n = 0;
  for (const SentFrame& f : rig.remotes.Sent()) {
    if (f.id == remote && nevr_evr_codec::FirstSymbol(f.data) == symbol) ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------------------------
// Identity: the Nth connection is config, login, matchmaker; all three share one session remote.
void TestConnectionIdentity() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  const auto opens = rig.remotes.Opens();
  QCHECK(opens.size() == 1);  // config, login and matchmaker share one session remote
  if (opens.size() == 1) {
    QCHECK(opens[0].connIdx == 0 && opens[0].role == Role::Config);  // the first connection opened it
    QCHECK(opens[0].session);
    QCHECK(!opens[0].standaloneMatchmaker);
  }
  QCHECK(rig.router->GetStats().games == 3);
  QCHECK(rig.router->GetStats().remotes == 1);
}

// Failure caught: the remote of a standalone matchmaker (one that arrived before any login connection) ending
// being treated as the end of the session, which would reset connection numbering and close the other sockets.
void TestStandaloneMatchmakerRemoteEndDoesNotEndTheSession() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1, Hinted(Role::Matchmaker));  // no session yet: a remote of its own
  rig.router->OnGameOpen(2, Hinted(Role::Config));
  rig.router->OnGameOpen(3, Hinted(Role::Login));
  const auto opens = rig.remotes.Opens();
  QCHECK(opens.size() == 2);
  if (opens.size() != 2) return;
  QCHECK(opens[0].standaloneMatchmaker && !opens[0].session);
  QCHECK(opens[1].session);
  rig.router->OnRemoteClose(opens[0].remote, 1000);
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1 && closes[0].id == 1);  // only the matchmaker socket
  QCHECK(rig.router->GetStats().nextConnIdx == 3);   // numbering untouched
  QCHECK(rig.router->GetStats().remotes == 1);
}

// ---------------------------------------------------------------------------------------------
// Login injection ordering. Failure caught: a game frame reaching the server before the LoginRequest.
void TestLoginIsFirstThenQueuedFramesInOrder() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId loginRemote = SessionRemote(rig);
  const std::string f1 = Msg(kSymSomething, "one"), f2 = Msg(kSymSomething, "two");
  rig.router->OnGameFrame(2, f1, true);
  rig.router->OnGameFrame(2, f2, true);
  QCHECK(rig.remotes.Sent().empty());  // nothing before the remote opens
  QCHECK(rig.router->GetStats().pendingFrames == 2);
  rig.router->OnRemoteOpen(loginRemote);
  const auto sent = rig.remotes.Sent();
  QCHECK(sent.size() == 3);
  if (sent.size() == 3) {
    QCHECK(sent[0].data == kLogin);
    QCHECK(sent[1].data == f1);
    QCHECK(sent[2].data == f2);
    QCHECK(sent[0].id == loginRemote);
  }
  QCHECK(rig.router->GetStats().pendingFrames == 0);
}

// Failure caught: a frame the game sends WHILE the login frame is being built overtaking it. The builder
// runs with no lock held, so it can (and here does) re-enter the router.
void TestFrameDuringLoginBuildStaysBehindLogin() {
  Rig* rigPtr = nullptr;
  const std::string late = Msg(kSymSomething, "late");
  Options o;
  o.buildLogin = [&]() {
    rigPtr->router->OnGameFrame(2, late, true);  // re-entrant call from inside the builder
    return std::optional<std::string>(kLogin);
  };
  Rig rig(std::move(o));
  rigPtr = &rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  rig.router->OnRemoteOpen(SessionRemote(rig));
  const auto sent = rig.remotes.Sent();
  QCHECK(sent.size() == 2);
  if (sent.size() == 2) {
    QCHECK(sent[0].data == kLogin);
    QCHECK(sent[1].data == late);
  }
}

// Failure caught: a second login on the same session (OnRemoteOpen delivered twice).
void TestLoginInjectedOncePerLoginSessionOnly() {
  int builds = 0;
  Options o;
  o.buildLogin = [&]() {
    ++builds;
    return std::optional<std::string>(kLogin);
  };
  Rig rig(std::move(o));
  OpenThree(rig);
  QCHECK(rig.remotes.Opens().size() == 1);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);  // the login connection is attached: the login goes out
  rig.router->OnRemoteOpen(session);  // duplicate event
  QCHECK(builds == 1);
  int logins = 0;
  for (const auto& s : rig.remotes.Sent()) logins += (s.data == kLogin);
  QCHECK(logins == 1);
  for (const auto& s : rig.remotes.Sent()) QCHECK(s.id == session);
  // The matchmaker rides the login remote and its frames go out after the login, without another login.
  rig.router->OnGameFrame(3, Msg(kSymSomething, "mm"), true);
  QCHECK(builds == 1);
  QCHECK(rig.remotes.Sent().back().id == session);
}

// Failure caught: fabricating a login when the identity is missing. No frame, an Error line, and the
// queued frames still flow (the PC bridge does the same).
void TestNoLoginFrameWhenIdentityMissing() {
  Options o;
  o.buildLogin = []() { return std::optional<std::string>(); };
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  rig.router->OnGameFrame(2, Msg(kSymSomething, "q"), true);
  rig.router->OnRemoteOpen(SessionRemote(rig));
  const auto sent = rig.remotes.Sent();
  QCHECK(sent.size() == 1);
  QCHECK(rig.logs.Has("login NOT injected", static_cast<int>(LogLevel::Error)));
  for (const auto& s : sent) QCHECK(nevr_evr_codec::FirstSymbol(s.data) != nevr_evr_codec::kSymLoginRequest);
}

// A throwing builder is the same as no identity, not a crash.
void TestThrowingBuilderIsContained() {
  Options o;
  o.buildLogin = []() -> std::optional<std::string> { throw std::runtime_error("boom"); };
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  rig.router->OnRemoteOpen(SessionRemote(rig));
  QCHECK(rig.logs.Has("login NOT injected"));
}

// ---------------------------------------------------------------------------------------------
// Remote close.
// Failure caught: (a) a game socket left open on a dead session, (b) the next connection being counted
// as a matchmaker on the dead session, (c) the close being issued with the router lock held (the #70
// deadlock): the fake Close asks the router for its stats from another thread and must get an answer.
void TestRemoteCloseClosesGameSocketsWithoutTheLock() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  std::atomic<int> probes{0}, answered{0};
  std::vector<std::thread> probeThreads;  // joined after the router call: a probe blocked on a held lock
                                          // can only finish once the closing thread releases it
  rig.games.onClose = [&](GameId) {
    ++probes;
    auto answer = std::make_shared<std::promise<void>>();
    std::future<void> done = answer->get_future();
    probeThreads.emplace_back([&rig, answer]() {
      rig.router->GetStats();  // takes the router lock
      answer->set_value();
    });
    if (done.wait_for(std::chrono::seconds(2)) == std::future_status::ready) ++answered;
  };
  rig.router->OnRemoteClose(session, 1006);
  for (auto& t : probeThreads) t.join();
  auto closes = rig.games.Closes();
  std::vector<uint64_t> ids;
  for (const auto& c : closes) ids.push_back(c.id);
  QCHECK(closes.size() == 3);  // config (1), login (2) and matchmaker (3) all share the session
  QCHECK(std::find(ids.begin(), ids.end(), 1) != ids.end());
  QCHECK(std::find(ids.begin(), ids.end(), 2) != ids.end());
  QCHECK(std::find(ids.begin(), ids.end(), 3) != ids.end());
  for (const auto& c : closes) QCHECK(c.code == kCloseGoingAway);
  QCHECK(probes == 3);
  QCHECK(answered == probes);  // the lock was free during every Close
  QCHECK(rig.router->GetStats().nextConnIdx == 1);
  // The game's next connection is a fresh login on a fresh session remote.
  rig.router->OnGameClose(1);
  rig.router->OnGameClose(2);
  rig.router->OnGameClose(3);
  rig.router->OnGameOpen(4);
  const auto after = rig.remotes.Opens();
  QCHECK(after.size() == 2);
  if (after.size() == 2) {
    QCHECK(after[1].role == Role::Login);
    QCHECK(after[1].session);
    QCHECK(after[1].remote != session);
  }
}

// Failure caught: Close() re-entering the router (a transport reporting the close synchronously) deadlocking.
void TestCloseMayReenterRouter() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.games.onClose = [&](GameId game) { rig.router->OnGameClose(game); };
  rig.router->OnRemoteClose(login, 1000);
  QCHECK(rig.router->GetStats().games == 0);  // the config game shared the session
}

// Failure caught: a remote error before it ever opened (connect refused, TLS verification failed) leaving
// the game waiting on a session that will never open.
void TestRemoteErrorBeforeOpenClosesGame() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  rig.router->OnGameFrame(2, Msg(kSymSomething), true);
  rig.router->OnRemoteError(SessionRemote(rig), 0, "tls verification failed");
  QCHECK(rig.games.Closes().size() == 2);  // the config and login connections share the session
  QCHECK(rig.remotes.Sent().empty());  // nothing, in particular no login, leaves on a failed session
  QCHECK(rig.logs.Has("tls verification failed"));
  QCHECK(rig.logs.Has("login session forgotten"));
}

// A remote that cannot even be started ends the session the same way.
void TestRemoteOpenRefusedClosesGame() {
  Rig rig(WithLogin(kLogin));
  rig.remotes.openResult = false;
  rig.router->OnGameOpen(1);
  QCHECK(rig.games.Closes().size() == 1);
  if (!rig.games.Closes().empty()) QCHECK(rig.games.Closes()[0].code == kCloseInternalError);
  QCHECK(rig.router->GetStats().remotes == 0);
}

// Failure caught: a game closing taking the session's remote with it, and the OTHER direction (game closes)
// not closing a remote only it used.
void TestGameCloseEndsItsStandaloneRemoteButNotTheSession() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1, Hinted(Role::Matchmaker));  // arrives first: a remote of its own
  rig.router->OnGameOpen(2, Hinted(Role::Config));
  rig.router->OnGameOpen(3, Hinted(Role::Login));
  const auto opens = rig.remotes.Opens();
  QCHECK(opens.size() == 2);
  if (opens.size() != 2) return;
  rig.router->OnGameClose(1);
  auto closes = rig.remotes.Closes();
  QCHECK(closes.size() == 1 && closes[0].id == opens[0].remote);
  rig.router->OnGameClose(2);  // config on the session
  QCHECK(rig.remotes.Closes().size() == 1);
  rig.router->OnGameClose(3);  // login game: the session stays for later connections
  QCHECK(rig.remotes.Closes().size() == 1);
  QCHECK(rig.router->GetStats().remotes == 1);
}

// ---------------------------------------------------------------------------------------------
// Routing of remote frames on the shared login session.
void TestRemoteFramesFollowTheActiveGameAndFallBack() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnRemoteFrame(login, Msg(kSymSomething, "a"), true);
  auto sent = rig.games.Sent();
  QCHECK(sent.size() == 1 && sent[0].id == 3);  // the newest connection on the session
  rig.router->OnGameClose(3);
  rig.router->OnRemoteFrame(login, Msg(kSymSomething, "b"), true);
  sent = rig.games.Sent();
  QCHECK(sent.size() == 2 && sent[1].id == 2);  // back to the login connection
  rig.router->OnGameClose(2);
  rig.router->OnRemoteFrame(login, Msg(kSymSomething, "c"), true);
  QCHECK(rig.games.Sent().size() == 2);
  QCHECK(rig.router->GetStats().droppedRemoteFrames == 1);
  QCHECK(rig.logs.Has("DROPPED"));
}

// LoginSuccess is forwarded and answered with a friend-list subscribe on the same remote; LoginFailure
// is forwarded and logged with numbers only.
void TestLoginSuccessAndFailureHandling() {
  Options withSubscribe = WithLogin(kLogin);
  withSubscribe.subscribeFriendList = true;  // the PC wiring; off by default
  Rig rig(std::move(withSubscribe));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);
  QCHECK(rig.games.Sent().size() == 1);
  QCHECK(rig.logs.Has("LOGIN SUCCESS"));
  const auto sent = rig.remotes.Sent();
  QCHECK(sent.size() == 2);  // login, subscribe
  if (sent.size() == 2) QCHECK(sent[1].data == nevr_evr_codec::BuildFriendListSubscribe());

  std::string failurePayload;
  // The codec exports no little-endian appender; the payload layout is three u64 fields, written out here.
  const auto appendLE64 = [&failurePayload](uint64_t v) {
    for (int i = 0; i < 8; ++i) failurePayload.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  };
  appendLE64(0);   // placeholder fields
  appendLE64(0);
  appendLE64(7);   // status
  failurePayload += "PRIVATE-SERVER-TEXT";
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoginFailure, failurePayload), true);
  QCHECK(rig.games.Sent().size() == 2);  // forwarded: the client retries
  QCHECK(rig.logs.Has("LOGIN FAILURE"));
  QCHECK(!rig.logs.Has("PRIVATE-SERVER-TEXT"));
}

// Failure caught: logging a token or a frame payload.
void TestNoSecretsInLogs() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(kSymSomething, kSecret), true);
  rig.router->OnRemoteFrame(login, Msg(kSymSomething, kSecret), true);
  rig.router->OnRemoteError(login, 401, "unauthorized");
  QCHECK(!rig.logs.Has(kSecret));
  QCHECK(!rig.logs.Has("LOGIN-PAYLOAD"));
  QCHECK(!rig.logs.lines.empty());
}

// ---------------------------------------------------------------------------------------------
// Limits.
// Failure caught: an oversized frame being forwarded (or silently dropped). Exactly the limit passes.
void TestOversizedGameFrame() {
  Options o = WithLogin(kLogin);
  o.limits.maxFrameBytes = 100;
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  const std::size_t before = rig.remotes.Sent().size();
  rig.router->OnGameFrame(2, std::string(100, 'a'), true);
  QCHECK(rig.remotes.Sent().size() == before + 1);
  rig.router->OnGameFrame(2, std::string(101, 'a'), true);
  QCHECK(rig.remotes.Sent().size() == before + 1);  // not forwarded
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1 && closes[0].id == 2 && closes[0].code == kCloseMessageTooBig);
  QCHECK(rig.logs.Has("frame too large", static_cast<int>(LogLevel::Error)));
  rig.router->OnGameFrame(2, std::string(10, 'a'), true);  // a closing socket forwards nothing more
  QCHECK(rig.remotes.Sent().size() == before + 1);
}

// An oversized frame from the service ends the session rather than reaching the game.
void TestOversizedRemoteFrame() {
  Options o = WithLogin(kLogin);
  o.limits.maxFrameBytes = 100;
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnRemoteFrame(login, std::string(101, 'a'), true);
  QCHECK(rig.games.Sent().empty());
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 2);  // config and login share the session
  for (const auto& c : closes) QCHECK(c.code == kCloseMessageTooBig);
  QCHECK(rig.remotes.Closes().size() == 1);
}

// Failure caught: an unbounded queue while the remote never opens.
void TestPendingQueueIsBounded() {
  Options o = WithLogin(kLogin);
  o.limits.maxPendingFrames = 3;
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  for (int i = 0; i < 3; ++i) rig.router->OnGameFrame(2, Msg(kSymSomething), true);
  QCHECK(rig.games.Closes().empty());
  rig.router->OnGameFrame(2, Msg(kSymSomething), true);
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1 && closes[0].code == kCloseTryAgainLater);
  QCHECK(rig.router->GetStats().pendingFrames == 0);  // the closed socket's queued frames leave with it
}

void TestPendingBytesAreBounded() {
  Options o = WithLogin(kLogin);
  o.limits.maxPendingBytes = 100;
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  rig.router->OnGameFrame(2, std::string(60, 'a'), true);
  rig.router->OnGameFrame(2, std::string(60, 'b'), true);
  QCHECK(rig.games.Closes().size() == 1);
  QCHECK(rig.router->GetStats().pendingFrames == 0);  // the closed socket's queued frames leave with it
}

// ---------------------------------------------------------------------------------------------
// Backpressure. Failure caught: reordering or losing frames when the transport reports it is full.
void TestRemoteBackpressureKeepsOrderAndResumes() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);  // login frame goes out
  QCHECK(rig.remotes.Sent().size() == 1);
  rig.remotes.script = {SendResult::WouldBlock};
  const std::string a = Msg(kSymSomething, "A"), b = Msg(kSymSomething, "B"), c = Msg(kSymSomething, "C");
  rig.router->OnGameFrame(2, a, true);  // blocked: stays queued
  rig.router->OnGameFrame(2, b, true);
  rig.router->OnGameFrame(2, c, true);
  QCHECK(rig.remotes.Sent().size() == 1);
  QCHECK(rig.router->GetStats().outboundBytes == a.size() + b.size() + c.size());
  const int callsWhileBlocked = rig.remotes.sendCalls;
  rig.router->OnGameFrame(2, c, true);
  QCHECK(rig.remotes.sendCalls == callsWhileBlocked);  // not hammering a transport that said it is full
  rig.router->OnRemoteWritable(login);
  const auto sent = rig.remotes.Sent();
  QCHECK(sent.size() == 5);
  if (sent.size() == 5) {
    QCHECK(sent[1].data == a);
    QCHECK(sent[2].data == b);
    QCHECK(sent[3].data == c);
    QCHECK(sent[4].data == c);
  }
  QCHECK(rig.router->GetStats().outboundBytes == 0);
}

// Failure caught: a transport that never drains growing the queue forever.
void TestRemoteBackpressureOverflowEndsTheSession() {
  Options o = WithLogin(kLogin);
  o.limits.maxOutboundBytes = 200;
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.remotes.script = {SendResult::WouldBlock};
  for (int i = 0; i < 20 && rig.games.Closes().empty(); ++i) rig.router->OnGameFrame(2, std::string(60, 'z'), true);
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 2);  // config and login share the session
  for (const auto& c : closes) QCHECK(c.code == kCloseTryAgainLater);
  QCHECK(rig.router->GetStats().outboundBytes == 0);
  QCHECK(rig.logs.Has("outbound buffer to the remote is full"));
}

// A send the transport reports as failed ends the session; it is not retried and not ignored.
void TestRemoteSendFailureEndsTheSession() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.remotes.script = {SendResult::Failed};
  rig.router->OnGameFrame(2, Msg(kSymSomething), true);
  QCHECK(rig.games.Closes().size() == 2);  // config and login share the session
  QCHECK(rig.router->GetStats().remotes == 0);
}

void TestGameBackpressureKeepsOrder() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.games.script = {SendResult::WouldBlock};
  const std::string a = Msg(kSymSomething, "A"), b = Msg(kSymSomething, "B");
  rig.router->OnRemoteFrame(login, a, true);
  rig.router->OnRemoteFrame(login, b, true);
  QCHECK(rig.games.Sent().empty());
  rig.router->OnGameWritable(2);
  const auto sent = rig.games.Sent();
  QCHECK(sent.size() == 2);
  if (sent.size() == 2) QCHECK(sent[0].data == a && sent[1].data == b);
}

void TestGameBackpressureOverflowClosesTheGame() {
  Options o = WithLogin(kLogin);
  o.limits.maxOutboundBytes = 100;
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.games.script = {SendResult::WouldBlock};
  for (int i = 0; i < 10 && rig.games.Closes().empty(); ++i) rig.router->OnRemoteFrame(login, std::string(60, 'q'), true);
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1 && closes[0].code == kCloseTryAgainLater);
}

// ---------------------------------------------------------------------------------------------
// Concurrency: producers on several threads, the remote draining; per-producer order must hold and
// nothing may be lost or duplicated.
void TestConcurrentProducersKeepPerSourceOrder() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  rig.router->OnGameOpen(3);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  constexpr int kPerThread = 400;
  std::vector<std::thread> threads;
  for (GameId game : {GameId(2), GameId(3)}) {
    threads.emplace_back([&rig, game]() {
      for (int i = 0; i < kPerThread; ++i) {
        rig.router->OnGameFrame(game, Msg(game, std::to_string(i)), true);
      }
    });
  }
  threads.emplace_back([&rig, login]() {
    for (int i = 0; i < kPerThread; ++i) rig.router->OnRemoteFrame(login, Msg(kSymSomething, std::to_string(i)), true);
  });
  for (auto& t : threads) t.join();
  int next2 = 0, next3 = 0;
  bool ordered = true;
  const auto sent = rig.remotes.Sent();
  for (const auto& s : sent) {
    const uint64_t sym = nevr_evr_codec::FirstSymbol(s.data);
    if (sym != 2 && sym != 3) continue;
    const std::string payload = s.data.substr(nevr_evr_codec::kHeaderSize);
    int& next = sym == 2 ? next2 : next3;
    if (payload != std::to_string(next)) ordered = false;
    ++next;
  }
  QCHECK(ordered);
  QCHECK(next2 == kPerThread && next3 == kPerThread);
  QCHECK(rig.games.Sent().size() == static_cast<std::size_t>(kPerThread));
}


// The Quest wiring leaves every injection option at its default, because the game's own (rewritten) login is
// the only login. Failure caught: any frame the game did not send reaching the service: a LoginRequest on
// the login connection, or a friend-list subscribe after LoginSuccess.
void TestQuestDefaultsInjectNothing() {
  Rig rig;  // default Options: no buildLogin, subscribeFriendList off
  OpenThree(rig);
  const auto opens = rig.remotes.Opens();
  QCHECK(opens.size() == 1);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  QCHECK(rig.remotes.Sent().empty());  // opening a session sends nothing by itself
  const std::string gameLogin = Msg(nevr_evr_codec::kSymLoginRequest, "the-game's-own-login");
  rig.router->OnGameFrame(2, gameLogin, true);
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);
  rig.router->OnGameFrame(3, Msg(kSymSomething, "mm"), true);
  const auto sent = rig.remotes.Sent();
  QCHECK(sent.size() == 2);  // exactly what the game sent, in order
  if (sent.size() == 2) {
    QCHECK(sent[0].data == gameLogin);
    QCHECK(sent[1].data == Msg(kSymSomething, "mm"));
  }
  QCHECK(!rig.logs.Has("login injected"));
}

// Failure caught: an unbounded number of matchmaker connections riding the login session (a local flood).
void TestMatchmakerConnectionsAreCapped() {
  Options o = WithLogin(kLogin);
  o.limits.maxMatchmakerConnections = 2;
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  rig.router->OnGameOpen(3);  // matchmaker 1
  rig.router->OnGameOpen(4);  // matchmaker 2
  QCHECK(rig.games.Closes().empty());
  rig.router->OnGameOpen(5);  // over the cap: refused, never registered
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1 && closes[0].id == 5 && closes[0].code == kCloseTryAgainLater);
  QCHECK(rig.router->GetStats().games == 4);
  QCHECK(rig.router->GetStats().nextConnIdx == 4);  // the refused connection consumed no number
  rig.router->OnGameClose(3);  // a slot frees up
  rig.router->OnGameOpen(6);
  QCHECK(rig.games.Closes().size() == 1);
  QCHECK(rig.router->GetStats().games == 4);
}

// A connection that arrives while a login session is established is a matchmaker on that session, never a
// second login: no new remote, no login frame. (Numbering only returns to "login" after the session ends.)
void TestExtraConnectionNeverBecomesASecondLogin() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  const auto before = rig.remotes.Opens();
  rig.router->OnRemoteOpen(SessionRemote(rig));
  for (GameId g = 10; g < 14; ++g) rig.router->OnGameOpen(g);
  QCHECK(rig.remotes.Opens().size() == before.size());  // none of them opened a remote
  int logins = 0;
  for (const auto& s : rig.remotes.Sent()) logins += (s.data == kLogin);
  QCHECK(logins == 1);
  QCHECK(rig.router->GetStats().remotes == 1);
}

// Shutdown closes everything once and later events are harmless.
void TestShutdown() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  rig.router->Shutdown();
  QCHECK(rig.games.Closes().size() == 3);
  QCHECK(rig.remotes.Closes().size() == 1);
  rig.router->Shutdown();
  QCHECK(rig.games.Closes().size() == 3);
  rig.router->OnGameFrame(2, Msg(kSymSomething), true);
  rig.router->OnRemoteFrame(1, Msg(kSymSomething), true);
  rig.router->OnRemoteOpen(2);
  rig.router->OnGameOpen(9);
  QCHECK(rig.games.Closes().size() == 4);  // a game that connects after shutdown is closed at once
}

// ---------------------------------------------------------------------------------------------
// Roles by first data frame, and routing by role.

// Failure caught: a symbol landing in the wrong role. Every config and lobby request the game opens a
// connection with is named; LogInRequestv2, silence and anything else are the login role.
void TestClassifyFirstFrameTable() {
  QCHECK(ClassifyFirstFrame(nevr_evr_codec::kSymConfigRequest) == Role::Config);
  const uint64_t lobby[] = {nevr_evr_codec::kSymMatchmakerStatusRequest, nevr_evr_codec::kSymFindSessionRequest,
                            nevr_evr_codec::kSymCreateSessionRequest,    nevr_evr_codec::kSymJoinSessionRequest,
                            nevr_evr_codec::kSymDirectoryRequest,        nevr_evr_codec::kSymPendingSessionCancel,
                            nevr_evr_codec::kSymPlayerSessionsRequest,   nevr_evr_codec::kSymLobbyPingResponse};
  for (const uint64_t symbol : lobby) QCHECK(ClassifyFirstFrame(symbol) == Role::Matchmaker);
  QCHECK(ClassifyFirstFrame(nevr_evr_codec::kSymLoginRequest) == Role::Login);
  QCHECK(ClassifyFirstFrame(0) == Role::Login);  // nothing sent yet, or a frame shorter than a header
  QCHECK(ClassifyFirstFrame(kSymSomething) == Role::Login);
  // The literal values are the ones the game sends (libr15/libpnsovr, recorded in ADR 0003).
  QCHECK(nevr_evr_codec::kSymConfigRequest == 0x82869f0b37eb4378ULL);
  QCHECK(nevr_evr_codec::kSymFindSessionRequest == 0x312c2a01819aa3f5ULL);
  QCHECK(nevr_evr_codec::kSymConnectionUnrequire == 0x43e6963ac76beee4ULL);
}

// The smoke failure (#239): the config connection fails at boot (no account token yet), the login
// connection stays, and after the player signs in the game opens a NEW config connection. By order that
// connection is a matchmaker riding the login session, and the login session's profile reply went to it
// (the newest socket) instead of the login connection. Failure caught: that mis-role and mis-delivery.
void TestSmokeSequenceNewConfigSocketIsConfigAndProfileReplyReachesLogin() {
  Rig rig;
  rig.router->OnGameOpen(1);  // boot config connection
  const RemoteId bootConfig = SessionRemote(rig);
  rig.router->OnGameFrame(1, Msg(nevr_evr_codec::kSymConfigRequest), true);
  rig.router->OnRemoteError(bootConfig, 0, "no account token");  // fail-fast: the router closes game 1
  rig.router->OnGameClose(1);
  rig.router->OnGameOpen(2);  // the login connection (silent until the game sends its LogInRequest)
  const RemoteId login = SessionRemote(rig);
  QCHECK(rig.remotes.Opens().size() == 2);
  QCHECK(rig.remotes.Opens().back().role == Role::Login && rig.remotes.Opens().back().session);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest, "login"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);

  rig.router->OnGameOpen(3);  // the post-login config connection: provisionally a matchmaker (order)
  QCHECK(rig.remotes.Opens().size() == 2);  // sharing the session: no remote yet
  rig.router->OnGameFrame(3, Msg(nevr_evr_codec::kSymConfigRequest), true);
  QCHECK(rig.remotes.Opens().size() == 2);  // relabelled in place: still the one session remote
  QCHECK(SessionRemote(rig) == login);

  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConfigSuccess, "config"), true);
  const auto sent = rig.games.Sent();
  QCHECK(sent.size() == 3);  // LoginSuccess, profile, config
  if (sent.size() == 3) {
    QCHECK(sent[0].id == 2);
    QCHECK(sent[1].id == 2);  // the profile reply is the login connection's
    QCHECK(sent[2].id == 3);  // the config reply is the config connection's
  }
  // The config request went to the session remote, the one the login rides.
  QCHECK(CountUpstream(rig, login, nevr_evr_codec::kSymConfigRequest) == 1);
  QCHECK(rig.logs.Has("first frame symbol=0x82869f0b37eb4378: matchmaker -> config"));
}

// The login reply as nakama sends it: one frame, [LogInSuccess, Unrequire, LoginSettings].
std::string LoginReplyFrame() {
  return Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')) + Msg(nevr_evr_codec::kSymConnectionUnrequire, "") +
         Msg(nevr_evr_codec::kSymLoginSettings, "settings");
}

// Failure caught: lobby replies going to the login socket (or login replies to a matchmaker), and an
// Unrequire reaching a connection that has nothing outstanding. The game's own count is 8 bits and wraps
// below zero, which later raises "connection lost" on the login socket.
void TestServerFramesRouteByRole() {
  Rig rig;
  OpenThree(rig);  // config 1, login 2, matchmaker 3 (provisional)
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest), true);
  rig.router->OnGameFrame(3, Msg(nevr_evr_codec::kSymFindSessionRequest), true);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymDocumentRequest), true);
  rig.router->OnRemoteFrame(login, Msg(kSymLobbySessionSuccess, "lobby"), true);                      // -> 3
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile"), true);   // -> 2, no Unrequire
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymDocumentSuccess, "doc"), true);                  // -> 2, then its Unrequire
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);                 // -> 2 (document outstanding)
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymOtherUserProfileSuccess, "other"), true);        // -> 2
  rig.router->OnRemoteFrame(login, Msg(kSymLobbySessionSuccess, "lobby2"), true);                     // -> 3
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);                 // bare: dropped
  const auto sent = rig.games.Sent();
  const uint64_t expected[] = {3, 2, 2, 2, 2, 3};
  QCHECK(sent.size() == 6);
  for (std::size_t i = 0; i < sent.size() && i < 6; ++i) QCHECK(sent[i].id == expected[i]);
  QCHECK(rig.router->GetStats().droppedUnrequires == 1);
}

// Failure caught (#239 review M1): the service sends a reply and its Unrequire as separate frames, from
// concurrent goroutines, so frames interleave: [UpdateProfileSuccess, ping, Unrequire, Unrequire]. Each
// Unrequire goes to the connection that owes it, in the order the paired messages went out: the update's to
// the login connection, the ping's to the matchmaker (which answered the ping and so has it outstanding).
void TestInterleavedUnrequiresReachTheConnectionThatOwesThem() {
  Rig rig;
  OpenThree(rig);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest), true);
  rig.router->OnGameFrame(3, Msg(nevr_evr_codec::kSymLobbyPingResponse), true);  // flagged on the matchmaker connection
  rig.router->OnRemoteFrame(login, LoginReplyFrame(), true);                // -> 2; its Unrequire is inside
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymUpdateProfile, "update"), true);
  const std::string unrequire = Msg(nevr_evr_codec::kSymConnectionUnrequire, "");
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymUpdateProfileSuccess, "ok"), true);  // goroutine A
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLobbyPingRequest, "ping"), true);    // goroutine B
  rig.router->OnRemoteFrame(login, unrequire, true);                                       // A's -> 2
  rig.router->OnRemoteFrame(login, unrequire, true);                                       // B's -> 3
  const auto sent = rig.games.Sent();
  const uint64_t expected[] = {2, 2, 3, 2, 3};
  QCHECK(sent.size() == 5);
  for (std::size_t i = 0; i < sent.size() && i < 5; ++i) QCHECK(sent[i].id == expected[i]);
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
}

// Failure caught (RE M2): ping discovery sends LobbyPingRequest on the login session right after the login
// and an Unrequire after it. With no matchmaker connection the ping must not fall to the login connection, whose
// count would go below zero. The ping and the Unrequire that follows it are dropped; the login connection's
// count is untouched, so its own update's Unrequire still arrives.
void TestPingDiscoveryAfterLoginWithoutAMatchmakerDoesNotWrapTheLoginCount() {
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest), true);
  rig.router->OnRemoteFrame(login, LoginReplyFrame(), true);                                 // -> 2, count 1 -> 0
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLobbyPingRequest, "ping"), true);       // no matchmaker: dropped
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);        // the ping's: dropped
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymUpdateProfile, "update"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymUpdateProfileSuccess, "ok"), true);     // -> 2
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);        // -> 2 (count 1 -> 0)
  const auto sent = rig.games.Sent();
  QCHECK(sent.size() == 3);
  for (const auto& f : sent) QCHECK(f.id == 2);
  QCHECK(!sent.empty() && nevr_evr_codec::FirstSymbol(sent[0].data) == nevr_evr_codec::kSymLoginSuccess);
  QCHECK(sent.size() == 3 && nevr_evr_codec::FirstSymbol(sent[1].data) == nevr_evr_codec::kSymUpdateProfileSuccess);
  QCHECK(sent.size() == 3 && nevr_evr_codec::FirstSymbol(sent[2].data) == nevr_evr_codec::kSymConnectionUnrequire);
  QCHECK(rig.router->GetStats().droppedUnrequires == 1);
  QCHECK(rig.router->GetStats().droppedRemoteFrames == 1);  // the ping
}

// Failure caught (RE M2): ChannelInfoResponse is accepted on any peer by the game, so it went to the newest
// connection, a matchmaker, while its Unrequire's count is on the login connection that sent the request.
void TestChannelInfoResponseGoesToTheLoginConnectionWhileAMatchmakerExists() {
  Rig rig;
  OpenThree(rig);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest), true);
  rig.router->OnGameFrame(3, Msg(nevr_evr_codec::kSymFindSessionRequest), true);
  rig.router->OnRemoteFrame(login, LoginReplyFrame(), true);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymChannelInfoRequest, "channel"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymChannelInfoResponse, "info"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);
  const auto sent = rig.games.Sent();
  QCHECK(sent.size() == 3);
  for (const auto& f : sent) QCHECK(f.id == 2);  // nothing reaches the matchmaker
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
}

// Failure caught: requests the game sends without the require flag (LogOut, TelemetryEvent, the matchmaker
// status request) counted as outstanding, so an Unrequire nothing was owed reached the connection. And an
// Unrequire after a paired reply to a connection with nothing outstanding (no request was made) is dropped.
void TestUnflaggedRequestsDoNotCountAndAnExtraUnrequireIsDropped() {
  Rig rig;
  OpenThree(rig);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLogOut), true);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymTelemetryEvent), true);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymRemoteLogSet), true);
  rig.router->OnGameFrame(3, Msg(nevr_evr_codec::kSymMatchmakerStatusRequest), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymDocumentSuccess, "doc"), true);   // -> 2; nothing was asked
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);  // dropped: nothing outstanding
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);  // dropped: nothing owed
  QCHECK(rig.games.Sent().size() == 1);
  QCHECK(rig.router->GetStats().droppedUnrequires == 2);
}

// Failure caught: the Unrequire inside the login reply frame not lowering the login connection's count, so the
// login request stayed outstanding and an Unrequire nothing was owed (after a document nobody asked for) went
// through and wrapped the game's count.
void TestTheUnrequireInsideTheLoginReplyFrameLowersTheLoginCount() {
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest), true);
  rig.router->OnRemoteFrame(login, LoginReplyFrame(), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymDocumentSuccess, "doc"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);
  QCHECK(rig.games.Sent().size() == 2);
  QCHECK(rig.router->GetStats().droppedUnrequires == 1);
}

// Failure caught (#239 second review M1): nakama sends every login failure as a LoginFailure frame and then a
// standalone Unrequire. With LoginFailure missing from the messages the router pairs with an Unrequire, that
// Unrequire was dropped and the login request stayed outstanding, so a later Unrequire nothing was owed (after
// a document nobody asked for) went through and wrapped the game's count.
void TestALoginFailureAndItsUnrequireLowerTheLoginCount() {
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest), true);
  std::string failure;
  for (int i = 0; i < 3; ++i) failure.append(8, '\0');  // the three u64 fields; the status is not read here
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoginFailure, failure + "text"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);  // the login's: delivered
  QCHECK(rig.games.Sent().size() == 2);
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
  // The login count is back to zero: an Unrequire after a document nobody asked for is dropped.
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymDocumentSuccess, "doc"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);
  QCHECK(rig.games.Sent().size() == 3);
  QCHECK(rig.router->GetStats().droppedUnrequires == 1);
}

// Failure caught (#239 final review): with nakama's DisableLoginMessage branch a login failure and its Unrequire
// are followed by a login that still succeeds, so the success frame's embedded Unrequire arrives with the login
// request already covered. The router cannot take an Unrequire out of a frame: it reaches the game, and the
// count the router cannot cover is counted, where production reports it, instead of passing unseen.
void TestAnEmbeddedUnrequireTheCountCannotCoverIsCounted() {
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest), true);
  std::string failure;
  for (int i = 0; i < 3; ++i) failure.append(8, '\0');
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoginFailure, failure + "text"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);  // covers the login request
  QCHECK(rig.router->GetStats().unmatchedEmbeddedUnrequires == 0);
  rig.router->OnRemoteFrame(login, LoginReplyFrame(), true);  // the login that still succeeds: its Unrequire is unowed
  QCHECK(rig.games.Sent().size() == 3);  // the frame is delivered whole
  QCHECK(rig.router->GetStats().unmatchedEmbeddedUnrequires == 1);
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
  // A frame the router drops with its Unrequire inside counts it too (a ping with no matchmaker, batched).
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLobbyPingRequest, "ping") +
                                       Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);
  QCHECK(rig.games.Sent().size() == 3);
  QCHECK(rig.router->GetStats().unmatchedEmbeddedUnrequires == 2);
}

// The config connection's Unrequire follows the same rule: it lowers the config connection's own count.
void TestConfigConnectionUnrequireNeverExceedsItsRequests() {
  Rig rig;
  OpenThree(rig);
  const RemoteId config = rig.remotes.Opens()[0].remote;
  rig.router->OnRemoteOpen(config);
  rig.router->OnGameFrame(1, Msg(nevr_evr_codec::kSymConfigRequest), true);
  rig.router->OnRemoteFrame(config, Msg(nevr_evr_codec::kSymConfigSuccess, "config"), true);
  rig.router->OnRemoteFrame(config, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);  // delivered: 1 -> 0
  rig.router->OnRemoteFrame(config, Msg(nevr_evr_codec::kSymConnectionUnrequire, ""), true);  // dropped
  QCHECK(rig.games.Sent().size() == 2);
  QCHECK(rig.router->GetStats().droppedUnrequires == 1);
}

// Failure caught: an unlisted message on a matchmaker connection moving it to the login role and taking the
// login session's replies. Only LogInRequestv2 moves a connection to login.
void TestUnknownFirstFrameKeepsTheProvisionalRole() {
  Rig rig;
  OpenThree(rig);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(3, Msg(kSymSomething, "unlisted"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile"), true);
  const auto sent = rig.games.Sent();
  QCHECK(sent.size() == 1 && sent[0].id == 2);
  QCHECK(!rig.logs.Has("-> login"));
}

// Failure caught: the game's reconnected login connection (the old one closed with nothing outstanding)
// being left a matchmaker, so the login replies had no login socket to reach.
void TestReconnectedLoginConnectionTakesOverTheSession() {
  Rig rig;
  OpenThree(rig);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameClose(2);  // the login connection goes
  rig.router->OnGameOpen(4);   // the game's new login connection: the session has no login connection, so it is one
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymLoginRequest), true);
  QCHECK(rig.remotes.Opens().size() == 1);  // it rides the live session: no second remote
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);
  rig.router->OnRemoteFrame(login, Msg(kSymLobbySessionSuccess, "lobby"), true);
  const auto sent = rig.games.Sent();
  QCHECK(sent.size() == 2);
  if (sent.size() == 2) {
    QCHECK(sent[0].id == 4);  // the login reply: the new login connection
    QCHECK(sent[1].id == 3);  // the lobby reply: the matchmaker, still
  }
  QCHECK(rig.logs.Has("(login) takes over login session"));
}

// Failure caught: a login reply, with the login connection gone, being handed to whichever socket is
// newest. It is dropped and counted; lobby traffic still reaches the matchmaker.
void TestLoginReplyWithoutALoginConnectionIsDropped() {
  Rig rig;
  OpenThree(rig);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameClose(2);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile"), true);
  QCHECK(rig.games.Sent().empty());
  QCHECK(rig.router->GetStats().droppedRemoteFrames == 1);
  rig.router->OnRemoteFrame(login, Msg(kSymLobbySessionSuccess, "lobby"), true);
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 3);
}

// Failure caught: a connection that was given the login role by order but opens with a config request keeping
// the exemption, or being released and re-attached (a remote opened or closed). It is relabelled in place.
void TestProvisionalLoginThatSendsConfigIsRelabelledInPlace() {
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);  // provisional login, sharing the session game 1 opened
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymConfigRequest), true);
  QCHECK(rig.remotes.Opens().size() == 1);  // nothing opened
  QCHECK(rig.remotes.Closes().empty());     // nothing released
  QCHECK(rig.games.Closes().empty());
  QCHECK(rig.router->GetStats().pendingFrames == 1);  // the config request waits for the session to open
  QCHECK(!Exempt(rig, 2));  // no longer the login connection
  QCHECK(rig.router->GetStats().nextConnIdx == 2);
  rig.router->OnGameOpen(3);  // the session has no login connection: this one is the login again
  QCHECK(rig.remotes.Opens().size() == 1);
  QCHECK(Exempt(rig, 3));
}

// Failure caught: the provisional login connection of a session other connections ride being kept as the
// login by a config first frame ("role stays login"). It is relabelled in place, the session untouched.
void TestLoginConnectionWithSharersIsRelabelledInPlace() {
  Rig rig;
  OpenThree(rig);
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymConfigRequest), true);  // the provisional login is config
  QCHECK(rig.remotes.Opens().size() == 1);
  QCHECK(!rig.logs.Has("role stays"));
  QCHECK(!Exempt(rig, 2));
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile"), true);
  QCHECK(rig.games.Sent().empty());  // no login connection on the session: dropped, and counted
  QCHECK(rig.router->GetStats().droppedRemoteFrames == 1);
  rig.router->OnGameFrame(3, Msg(nevr_evr_codec::kSymLoginRequest), true);  // the real login names itself
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile"), true);
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 3);
}

// Failure caught: a second connection that names itself the login (LogInRequestv2) while the first login
// connection is still open leaving the old one in charge of the login replies.
void TestLoginRequestOnAnotherConnectionTakesOverFromALiveLogin() {
  Rig rig;
  OpenThree(rig);  // config 1, login 2, matchmaker 3
  const RemoteId login = SessionRemote(rig);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameOpen(4);  // a live login exists: provisionally a matchmaker
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymLoginRequest), true);
  QCHECK(rig.remotes.Opens().size() == 1);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile"), true);
  rig.router->OnRemoteFrame(login, Msg(kSymLobbySessionSuccess, "lobby"), true);
  const auto sent = rig.games.Sent();
  QCHECK(sent.size() == 2);
  if (sent.size() == 2) {
    QCHECK(sent[0].id == 4);  // the new login connection
    QCHECK(sent[1].id == 3);  // lobby: the newest matchmaker connection (the demoted game 2 is older)
  }
}

// ---------------------------------------------------------------------------------------------
// A held login: the login connection waits for the account instead of failing.

struct Gate {
  std::atomic<int> value{static_cast<int>(LoginGate::Awaiting)};
  void Set(LoginGate g) { value = static_cast<int>(g); }
  LoginGateFn Fn() {
    return [this]() { return static_cast<LoginGate>(value.load()); };
  }
};

// Failure caught: the login connection failing (1011) while no account exists, or the session never opening
// once the account is there. The config connection opens its remote at once and is refused (the bridge has no
// token to open it with: 1011, as before the session was shared); the login connection that follows holds its
// session: no remote, no close, the transport is told to keep the login connection open, and what the game
// sends meanwhile waits in order; when the account is there the remote opens and the frames follow.
void TestHeldSessionOpensWhenTheAccountAppears() {
  Gate gate;
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.remotes.openResult = false;  // what the bridge answers when there is no account token
  rig.router->OnGameOpen(1);       // config: opens at once, refused
  QCHECK(rig.remotes.Opens().size() == 1 && rig.remotes.Opens()[0].role == Role::Config);
  QCHECK(rig.games.Closes().size() == 1 && rig.games.Closes()[0].id == 1);
  QCHECK(rig.router->GetStats().heldRemotes == 0);
  rig.remotes.openResult = true;
  rig.router->OnGameOpen(2);  // login: holds its session
  QCHECK(rig.remotes.Opens().size() == 1);  // still only config's attempt
  QCHECK(rig.games.Closes().size() == 1);
  QCHECK(rig.router->GetStats().heldRemotes == 1);
  const auto holds = rig.games.Holds();
  QCHECK(holds.size() == 1 && holds[0].first == 2 && holds[0].second);
  QCHECK(rig.logs.Has("(login) held"));
  const std::string loginRequest = Msg(nevr_evr_codec::kSymLoginRequest, "the-login");
  const std::string next = Msg(nevr_evr_codec::kSymConfigRequest, "after");
  rig.router->OnGameFrame(2, loginRequest, true);
  rig.router->OnGameFrame(2, next, true);
  QCHECK(rig.router->GetStats().pendingFrames == 2);
  rig.router->ReevaluateHeldLogins();  // still awaiting: nothing changes
  QCHECK(rig.remotes.Opens().size() == 1 && rig.router->GetStats().heldRemotes == 1);
  gate.Set(LoginGate::Ready);
  rig.router->ReevaluateHeldLogins();
  const auto opens = rig.remotes.Opens();
  QCHECK(opens.size() == 2);
  RemoteId login = kNoRemote;
  if (opens.size() == 2) {
    QCHECK(opens[1].role == Role::Login && opens[1].session && !opens[1].standaloneMatchmaker);
    login = opens[1].remote;
  }
  QCHECK(rig.router->GetStats().heldRemotes == 0);
  {
    // The login connection stays idle-exempt after the release: it is silent until the game's login.
    const auto after = rig.games.Holds();
    QCHECK(after.size() == 1 && after[0] == std::make_pair(GameId(2), true));
  }
  rig.router->ReevaluateHeldLogins();  // idempotent: no second open
  QCHECK(rig.remotes.Opens().size() == 2);
  rig.router->OnRemoteOpen(login);
  const auto sent = rig.remotes.Sent();
  QCHECK(sent.size() == 2);
  if (sent.size() == 2) QCHECK(sent[0].data == loginRequest && sent[1].data == next);
  QCHECK(rig.games.Closes().size() == 1);  // config's refusal only
  // Replies reach the login connection once the session is up.
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 2);
}

// Failure caught (#704): a config connection held with the session while the account is awaited, so the game
// never saw its config connect fail and never started its sign-in. While the account is awaited a config
// connection opens its remote at once, and the bridge's refusal closes it (1011); only the login connection
// that follows is held. A matchmaker riding the held session is not idle-exempt.
void TestConfigConnectionFailsAtOnceWhileTheAccountIsAwaited() {
  Gate gate;
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.remotes.openResult = false;  // what the bridge answers when there is no account token
  rig.router->OnGameOpen(1);       // config
  QCHECK(rig.remotes.Opens().size() == 1);  // it reached the transport: it was not held
  QCHECK(rig.router->GetStats().heldRemotes == 0);
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1);
  if (closes.size() == 1) {
    QCHECK(closes[0].id == 1 && closes[0].code == kCloseInternalError);
    QCHECK(closes[0].reason == "remote could not be started");
  }
  QCHECK(rig.logs.Has("(config) opened"));
  QCHECK(!rig.logs.Has("(config) held"));
  rig.router->OnGameOpen(2);  // login: held
  QCHECK(rig.remotes.Opens().size() == 1);  // the login did not call Open
  QCHECK(rig.router->GetStats().heldRemotes == 1);
  QCHECK(rig.logs.Has("(login) held"));
  rig.router->OnGameOpen(3);  // a matchmaker riding the held session is not idle-exempt
  for (const auto& h : rig.games.Holds()) QCHECK(h.first == 2);
  QCHECK(rig.router->GetStats().heldRemotes == 1);
}

// The other half of the pin: once the account exists the config connection opens the session and the login
// connection shares that one remote (#647), so config's first reply does not wait behind a second remote.
void TestConfigAndLoginShareOneRemoteOnceTheAccountExists() {
  Gate gate;
  gate.Set(LoginGate::Ready);
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);  // config: opens the session
  rig.router->OnGameOpen(2);  // login: shares it
  const auto opens = rig.remotes.Opens();
  QCHECK(opens.size() == 1);
  if (opens.size() == 1) QCHECK(opens[0].role == Role::Config && opens[0].session);
  QCHECK(rig.router->GetStats().remotes == 1);
  QCHECK(rig.router->GetStats().heldRemotes == 0);
  QCHECK(rig.games.Closes().empty());
  QCHECK(rig.logs.Has("(login) takes over login session remote"));
}

// Failure caught: a held session that is never released when the sign-in expires or fails: the game would wait
// forever on connections that can never log in. The hold ends with a close of every socket on the session
// (1011, the code and text config failed with before it was held) and the session is forgotten.
void TestHeldSessionIsClosedWhenTheAccountIsRefused() {
  Gate gate;
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.remotes.openResult = false;  // config is refused at once, as without an account
  rig.router->OnGameOpen(1);
  rig.remotes.openResult = true;
  rig.router->OnGameOpen(2);
  rig.router->OnGameOpen(3);
  QCHECK(rig.games.Closes().size() == 1);  // config's refusal
  gate.Set(LoginGate::Refused);
  rig.router->ReevaluateHeldLogins();
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 3);  // config's refusal, then every socket attached to the held session
  std::vector<uint64_t> ids;
  for (const auto& c : closes) {
    ids.push_back(c.id);
    QCHECK(c.code == kCloseInternalError);
    QCHECK(c.reason == (c.id == 1 ? "remote could not be started" : "the account the login needs will not be available"));
  }
  std::sort(ids.begin(), ids.end());
  QCHECK(ids == std::vector<uint64_t>({1, 2, 3}));
  QCHECK(rig.router->GetStats().heldRemotes == 0);
  QCHECK(rig.remotes.Opens().size() == 1);  // only config's refused attempt; the held session never opened
  QCHECK(rig.router->GetStats().nextConnIdx == 1);  // the next connection is a login again
}

// Failure caught: the game's login connection reconnecting while the account is still awaited. The
// replacement is the login connection of the same held session: held again, one remote, opened once.
void TestReplacementLoginConnectionIsHeldOnTheSameSession() {
  Gate gate;
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.remotes.openResult = false;  // config is refused at once
  rig.router->OnGameOpen(1);
  rig.remotes.openResult = true;
  rig.router->OnGameOpen(2);
  rig.router->OnGameClose(2);
  rig.router->OnGameOpen(3);
  QCHECK(rig.router->GetStats().heldRemotes == 1);
  {
    const auto held = rig.games.Holds();
    QCHECK(!held.empty() && held.back() == std::make_pair(GameId(3), true));
  }
  QCHECK(rig.games.Closes().size() == 1);  // config's refusal only
  gate.Set(LoginGate::Ready);
  rig.router->ReevaluateHeldLogins();
  QCHECK(rig.remotes.Opens().size() == 2);  // config's refused attempt, then the held session once
}

// Without a gate nothing is held, but the login connection is idle-exempt all the same.
void TestNoGateNeverHolds() {
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  QCHECK(rig.remotes.Opens().size() == 1 && rig.router->GetStats().heldRemotes == 0);
  // The login connection is idle-exempt with or without a gate (and only the login connection is).
  const auto holds = rig.games.Holds();
  QCHECK(holds.size() == 1 && holds[0] == std::make_pair(GameId(2), true));
  rig.router->ReevaluateHeldLogins();
}

// Failure caught (#239 review H1): the login connection is silent until the game's LogInRequest, which can come
// minutes after it connected. It must be exempt from the first-frame idle close from the moment it is known
// to be the login connection until it stops being it; config and matchmaker connections never are.
void TestOnlyTheLoginConnectionIsIdleExempt() {
  Rig rig;
  OpenThree(rig);  // config 1, login 2, matchmaker 3
  auto holds = rig.games.Holds();
  QCHECK(holds.size() == 1 && holds[0] == std::make_pair(GameId(2), true));
  // A connection that proves to be the config connection while provisionally the login loses the exemption.
  Rig other;
  other.router->OnGameOpen(1);
  other.router->OnGameOpen(2);  // provisional login: exempt
  other.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymConfigRequest), true);  // it was the config connection
  holds = other.games.Holds();
  QCHECK(holds.size() == 2 && holds[0] == std::make_pair(GameId(2), true) && holds[1] == std::make_pair(GameId(2), false));
  // A matchmaker that takes over the login role gets it.
  Rig third;
  OpenThree(third);
  third.router->OnGameOpen(4);
  third.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymLoginRequest), true);
  holds = third.games.Holds();
  bool fourExempt = false, twoReleased = false;
  for (const auto& h : holds) {
    fourExempt = fourExempt || h == std::make_pair(GameId(4), true);
    twoReleased = twoReleased || h == std::make_pair(GameId(2), false);  // the old login stops being exempt
  }
  QCHECK(fourExempt && twoReleased);
}

// ---------------------------------------------------------------------------------------------
// #578: one session, roles named by an adapter's hint or by the first frame, relabelled in place.
//
// Every test below states a rule the router had not yet been given when it was written. Each prints
// "FAILED in <label>" (stderr) after its failing checks, so a failing run names the case.

constexpr GameId kCfg = 1;  // the config socket's id in the permutation tests
constexpr GameId kLog = 2;  // the login socket's
constexpr GameId kMm = 3;   // the matchmaker socket's

GameId IdOf(Role role) {
  switch (role) {
    case Role::Config: return kCfg;
    case Role::Login: return kLog;
    case Role::Matchmaker: return kMm;
  }
  return kNoGame;
}

using Perm = std::array<Role, 3>;

// All 6 orders of the three roles.
std::vector<Perm> AllPerms() {
  std::array<int, 3> idx = {0, 1, 2};
  const Role roles[3] = {Role::Config, Role::Login, Role::Matchmaker};
  std::vector<Perm> out;
  do {
    out.push_back({roles[idx[0]], roles[idx[1]], roles[idx[2]]});
  } while (std::next_permutation(idx.begin(), idx.end()));
  return out;
}

std::string PermName(const Perm& p) {
  std::string s;
  for (std::size_t i = 0; i < p.size(); ++i) s += std::string(i == 0 ? "" : ",") + RoleName(p[i]);
  return s;
}

std::size_t IndexOf(const Perm& p, Role role) {
  for (std::size_t i = 0; i < p.size(); ++i) {
    if (p[i] == role) return i;
  }
  return p.size();
}

// Prints a line after the checks of a case that failed, so the stderr names the case.
class CaseScope {
 public:
  explicit CaseScope(std::string label) : label_(std::move(label)), before_(quest_test::Failures()) {}
  ~CaseScope() {
    if (quest_test::Failures() != before_) {
      std::fprintf(stderr, "FAILED in %s: %d check(s)\n", label_.c_str(), quest_test::Failures() - before_);
    }
  }
  CaseScope(const CaseScope&) = delete;
  CaseScope& operator=(const CaseScope&) = delete;

 private:
  std::string label_;
  int before_;
};

// The first data frame each role's socket sends.
std::string FirstFrameOf(Role role) {
  switch (role) {
    case Role::Config: return Msg(nevr_evr_codec::kSymConfigRequest, "cfg");
    case Role::Login: return Msg(nevr_evr_codec::kSymLoginRequest, "login");
    case Role::Matchmaker: return Msg(nevr_evr_codec::kSymFindSessionRequest, "find");
  }
  return std::string();
}



std::size_t ExemptCount(Rig& rig) {
  std::size_t n = 0;
  for (const GameId g : {kCfg, kLog, kMm, GameId(4), GameId(5)}) n += Exempt(rig, g) ? 1 : 0;
  return n;
}

// (game id, first symbol) of every frame the router delivered to a game, in delivery order.
using Delivery = std::pair<GameId, uint64_t>;
std::vector<Delivery> Delivered(Rig& rig) {
  std::vector<Delivery> out;
  for (const SentFrame& f : rig.games.Sent()) out.emplace_back(f.id, nevr_evr_codec::FirstSymbol(f.data));
  return out;
}


int CountUpstreamAnywhere(Rig& rig, uint64_t symbol) {
  int n = 0;
  for (const SentFrame& f : rig.remotes.Sent()) n += nevr_evr_codec::FirstSymbol(f.data) == symbol ? 1 : 0;
  return n;
}

const std::string kUnrequire = Msg(nevr_evr_codec::kSymConnectionUnrequire, "");

// A config request as the game sends it, [pad byte][JSON {"type", "id"}], and the service's two replies to it:
// the success carries the two symbols, the failure the two strings (nakama server/evr/config_request.go,
// config_success.go, config_failure.go).
std::string ConfigRequestFor(const std::string& type, const std::string& id) {
  return Msg(nevr_evr_codec::kSymConfigRequest, std::string(1, '\0') + "{\"type\":\"" + type + "\",\"id\":\"" + id + "\"}");
}
std::string ConfigRequestFor(const std::string& name) { return ConfigRequestFor(name, name); }
std::string ConfigSuccessFor(const std::string& type, const std::string& id) {
  std::string payload;
  for (const uint64_t v : {nevr_evr_codec::ConfigSymbol(type), nevr_evr_codec::ConfigSymbol(id)}) {
    for (int i = 0; i < 8; ++i) payload.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  return Msg(nevr_evr_codec::kSymConfigSuccess, payload + "zstd-resource");
}
std::string ConfigSuccessFor(const std::string& name) { return ConfigSuccessFor(name, name); }
std::string ConfigFailureFor(const std::string& type, const std::string& id) {
  return Msg(nevr_evr_codec::kSymConfigFailure, std::string(16, '\0') + "{\"type\":\"" + type + "\",\"identifier\":\"" + id +
                                                    "\",\"errorCode\":1,\"error\":\"resource not found\"}");
}
std::string ConfigFailureFor(const std::string& name) { return ConfigFailureFor(name, name); }

// One shared walk for the unhinted (Quest) cases: three sockets open in `openOrder` (each one the role its
// name says, but the router is told nothing), the first frames arrive in `frameOrder`, and the service
// answers on the one session remote. The rules pinned:
//   * the three sockets share ONE remote, marked as the session remote;
//   * a socket whose first frame corrects its provisional role is relabelled in place: no remote is opened,
//     closed or re-attached, no socket is closed;
//   * the idle exemption follows the real login socket once it is identified, and no other socket;
//   * config, login and lobby replies each reach their socket, each Unrequire the socket that owes it.
void RunUnhinted(const Perm& openOrder, const Perm& frameOrder, const std::string& label) {
  const CaseScope scope(label);
  Rig rig;
  for (const Role r : openOrder) rig.router->OnGameOpen(IdOf(r));
  QCHECK(rig.remotes.Opens().size() == 1);  // one remote for the three
  QCHECK(rig.remotes.Opens().empty() || rig.remotes.Opens()[0].session);
  QCHECK(rig.router->GetStats().remotes == 1);
  const RemoteId remote = SessionRemote(rig);
  rig.router->OnRemoteOpen(remote);
  // Before any first frame: the socket the router takes for the login (second by order) holds the exemption;
  // a real login socket the router has not identified is still idle-closable.
  QCHECK(Exempt(rig, IdOf(openOrder[1])));
  if (openOrder[1] != Role::Login) QCHECK(!Exempt(rig, kLog));

  for (const Role r : frameOrder) {
    rig.router->OnGameFrame(IdOf(r), FirstFrameOf(r), true);
    if (r == Role::Login) rig.router->OnGameFrame(kLog, Msg(nevr_evr_codec::kSymDocumentRequest, "doc"), true);
  }
  // Relabelled in place: nothing opened, nothing closed.
  QCHECK(rig.remotes.Opens().size() == 1);
  QCHECK(rig.remotes.Closes().empty());
  QCHECK(rig.games.Closes().empty());
  QCHECK(rig.router->GetStats().remotes == 1);
  // The idle hold sits on the real login socket and on no other.
  QCHECK(Exempt(rig, kLog));
  QCHECK(!Exempt(rig, kCfg));
  QCHECK(!Exempt(rig, kMm));
  QCHECK(ExemptCount(rig) == 1);
  // The game's four frames all went up the one remote.
  QCHECK(rig.remotes.Sent().size() == 4);
  for (const SentFrame& f : rig.remotes.Sent()) QCHECK(f.id == remote);

  rig.router->OnRemoteFrame(remote, Msg(nevr_evr_codec::kSymConfigSuccess, "config"), true);                 // -> config
  rig.router->OnRemoteFrame(remote, kUnrequire, true);                                       // -> config (its reply's)
  rig.router->OnRemoteFrame(remote, LoginReplyFrame(), true);                                // -> login (Unrequire inside)
  rig.router->OnRemoteFrame(remote, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "p"), true);  // -> login
  rig.router->OnRemoteFrame(remote, Msg(nevr_evr_codec::kSymDocumentSuccess, "d"), true);    // -> login
  rig.router->OnRemoteFrame(remote, kUnrequire, true);                                       // -> login (the document's)
  rig.router->OnRemoteFrame(remote, Msg(kSymLobbySessionSuccess, "lobby"), true);            // -> matchmaker
  rig.router->OnRemoteFrame(remote, Msg(nevr_evr_codec::kSymLobbyPingRequest, "ping"), true);  // -> matchmaker
  rig.router->OnRemoteFrame(remote, kUnrequire, true);                                       // -> matchmaker (the ping's)
  const std::vector<Delivery> expected = {
      {kCfg, nevr_evr_codec::kSymConfigSuccess},
      {kCfg, nevr_evr_codec::kSymConnectionUnrequire},
      {kLog, nevr_evr_codec::kSymLoginSuccess},
      {kLog, nevr_evr_codec::kSymLoggedInUserProfileSuccess},
      {kLog, nevr_evr_codec::kSymDocumentSuccess},
      {kLog, nevr_evr_codec::kSymConnectionUnrequire},
      {kMm, kSymLobbySessionSuccess},
      {kMm, nevr_evr_codec::kSymLobbyPingRequest},
      {kMm, nevr_evr_codec::kSymConnectionUnrequire},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
  QCHECK(rig.router->GetStats().unmatchedEmbeddedUnrequires == 0);
}

// Unhinted: all 6 open orders of config, login and matchmaker, the first frames arriving in open order.
// Failure caught: config getting a remote of its own (two remotes where there is one session), and a reply
// or an Unrequire landing on the wrong socket in any order.
void TestUnhintedAllOpenOrdersShareOneSession() {
  for (const Perm& order : AllPerms()) RunUnhinted(order, order, "unhinted open=" + PermName(order));
}

// The first frames arrive in a different order from the opens, so a socket's provisional role (by open
// order) is corrected by its first frame while the others hold the session. Failure caught: a correction
// that releases and re-attaches (a remote opened or closed, a socket closed, "role stays login").
void TestUnhintedFirstFrameOrderVariedSeparatelyFromOpenOrder() {
  for (const Perm& open : AllPerms()) {
    for (const Perm& frames : AllPerms()) {
      if (open == frames) continue;
      RunUnhinted(open, frames, "unhinted open=" + PermName(open) + " first-frames=" + PermName(frames));
    }
  }
}

// the idle exemption follows the role, provisional or confirmed. Login is opened first (provisionally
// config), config second (provisionally login, so exempt), a matchmaker third. When config's first frame
// names it, the exemption leaves it even though the matchmaker shares the session (today: "role stays
// login" and the exemption stays on the config socket). The real login takes it only once identified.
void TestIdleExemptionFollowsTheRoleThroughARelabel() {
  const CaseScope scope("idle exemption");
  Rig rig;
  rig.router->OnGameOpen(kLog);  // provisionally config
  rig.router->OnGameOpen(kCfg);  // provisionally login
  rig.router->OnGameOpen(kMm);
  QCHECK(Exempt(rig, kCfg));   // provisional login holds it
  QCHECK(!Exempt(rig, kLog));  // the real login, silent and unidentified, is still idle-closable
  rig.router->OnGameFrame(kCfg, FirstFrameOf(Role::Config), true);
  QCHECK(!Exempt(rig, kCfg));  // a provisional login that turned out to be config drops it
  rig.router->OnGameFrame(kLog, FirstFrameOf(Role::Login), true);
  QCHECK(Exempt(rig, kLog));   // the real login holds it once identified
  QCHECK(!Exempt(rig, kCfg));
  QCHECK(!Exempt(rig, kMm));
  QCHECK(ExemptCount(rig) == 1);
  QCHECK(rig.remotes.Opens().size() == 1);
  QCHECK(rig.remotes.Closes().empty());
}

// ---- hinted: the PC adapter names each socket's role at open ------------------------------------

Options PcOptions(int* builds) {
  Options o;
  o.buildLogin = [builds]() {
    ++*builds;
    return std::optional<std::string>(kLogin);
  };
  return o;
}


// Opens the sockets of `order` with their hints. When `remoteOpensEarly`, each remote is opened as soon as
// the router asks for it, so the session remote can open before the login socket attaches; otherwise they
// open once every socket is attached.
void RunHinted(const Perm& order, bool remoteOpensEarly, const std::string& label) {
  const CaseScope scope(label);
  int builds = 0;
  Rig rig(PcOptions(&builds));
  const std::size_t loginAt = IndexOf(order, Role::Login);
  const std::size_t mmAt = IndexOf(order, Role::Matchmaker);
  std::size_t opened = 0;  // remotes already given OnRemoteOpen
  bool mmSentEarly = false;
  const auto openNewRemotes = [&]() {
    const auto opens = rig.remotes.Opens();
    for (; opened < opens.size(); ++opened) rig.router->OnRemoteOpen(opens[opened].remote);
  };
  for (std::size_t i = 0; i < order.size(); ++i) {
    rig.router->OnGameOpen(IdOf(order[i]), Hinted(order[i]));
    if (remoteOpensEarly) {
      openNewRemotes();
      // No login is built until a login socket is attached to an open session.
      if (i < loginAt) QCHECK(builds == 0);
      if (i == loginAt) QCHECK(builds == 1);  // injected at attach, the session being open already
      if (order[i] == Role::Matchmaker && i < loginAt) {
        // A standalone matchmaker is not attached to the session, so it does not wait for a login that may
        // never come: its first frame goes straight up its own open remote.
        rig.router->OnGameFrame(kMm, FirstFrameOf(Role::Matchmaker), true);
        mmSentEarly = true;
        RemoteId own = kNoRemote;
        for (const auto& o : rig.remotes.Opens()) {
          if (o.standaloneMatchmaker) own = o.remote;
        }
        QCHECK(own != kNoRemote && CountUpstream(rig, own, nevr_evr_codec::kSymFindSessionRequest) == 1);
        QCHECK(builds == 0);
      }
    }
  }
  // One session remote; a matchmaker that came before the login socket had no session to share and
  // keeps a remote of its own.
  const auto opens = rig.remotes.Opens();
  const bool mmStandalone = mmAt < loginAt;
  QCHECK(opens.size() == (mmStandalone ? 2u : 1u));
  int sessions = 0;
  for (const auto& o : opens) {
    sessions += o.session ? 1 : 0;
    if (o.standaloneMatchmaker) QCHECK(!o.session);
  }
  QCHECK(sessions == 1);
  const RemoteId session = SessionRemote(rig);
  RemoteId mmRemote = session;
  for (const auto& o : opens) {
    if (o.standaloneMatchmaker) mmRemote = o.remote;
  }
  QCHECK(mmStandalone == (mmRemote != session));
  QCHECK(rig.router->GetStats().remotes == opens.size());

  // The game's first frames, sent before the remotes open when they have not yet.
  rig.router->OnGameFrame(kCfg, FirstFrameOf(Role::Config), true);
  rig.router->OnGameFrame(kLog, Msg(nevr_evr_codec::kSymLoggedInUserProfileRequest, "profile"), true);
  if (!mmSentEarly) rig.router->OnGameFrame(kMm, FirstFrameOf(Role::Matchmaker), true);
  openNewRemotes();

  // Exactly one login per session, injected on the session remote and nowhere else, and first on it.
  QCHECK(builds == 1);
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 1);
  QCHECK(CountUpstreamAnywhere(rig, nevr_evr_codec::kSymLoginRequest) == 1);
  const std::vector<SentFrame> up = rig.remotes.Sent();
  RemoteId firstOnSession = kNoRemote;
  uint64_t firstSymbol = 0;
  for (const SentFrame& f : up) {
    if (f.id == session) {
      firstSymbol = nevr_evr_codec::FirstSymbol(f.data);
      firstOnSession = f.id;
      break;
    }
  }
  // The login frame leads the session remote's traffic.
  QCHECK(firstOnSession == session && firstSymbol == nevr_evr_codec::kSymLoginRequest);
  // The login frame is ahead of the login socket's own frame and of the matchmaker's, if it shares the session.
  std::size_t loginPos = up.size(), profilePos = up.size(), findPos = up.size();
  for (std::size_t i = 0; i < up.size(); ++i) {
    if (up[i].id != session) continue;
    const uint64_t sym = nevr_evr_codec::FirstSymbol(up[i].data);
    if (sym == nevr_evr_codec::kSymLoginRequest) loginPos = i;
    if (sym == nevr_evr_codec::kSymLoggedInUserProfileRequest) profilePos = i;
    if (sym == nevr_evr_codec::kSymFindSessionRequest) findPos = i;
  }
  QCHECK(loginPos < profilePos);
  if (!mmStandalone) QCHECK(loginPos < findPos);

  // Each reply reaches its own socket.
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymConfigSuccess, "config"), true);                              // -> config
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);  // -> login
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "p"), true);      // -> login
  rig.router->OnRemoteFrame(mmRemote, Msg(kSymLobbySessionSuccess, "lobby"), true);                        // -> matchmaker
  const std::vector<Delivery> expected = {
      {kCfg, nevr_evr_codec::kSymConfigSuccess},
      {kLog, nevr_evr_codec::kSymLoginSuccess},
      {kLog, nevr_evr_codec::kSymLoggedInUserProfileSuccess},
      {kMm, kSymLobbySessionSuccess},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(builds == 1);
}

// Hinted, all 6 open orders, the remotes opening after every socket is attached.
// Failure caught: a remote per config/login socket, a second login, a login behind the game's frames.
void TestHintedAllOpenOrdersOneSessionOneLogin() {
  for (const Perm& order : AllPerms()) RunHinted(order, false, "hinted open=" + PermName(order));
}

// Hinted, all 6 open orders, each remote opening as soon as it is asked for, so the session remote opens
// before the login socket attaches in the orders where config (or a standalone matchmaker's neighbour) comes
// first. Failure caught: the login injected only at remote open (never, when the login socket attaches to an
// already open session), or injected twice (at attach and at open).
void TestHintedSessionRemoteOpeningBeforeTheLoginSocketAttaches() {
  for (const Perm& order : AllPerms()) RunHinted(order, true, "hinted early-open open=" + PermName(order));
}

// ---- the #239 sequence and the end of a session --------------------------------------------

// After login the config socket closes and the game opens a new one. It reattaches to the SAME session:
// no new remote, none closed, and its replies reach it while the login socket's still reach the login.
void TestConfigClosesAndReopensAfterLoginOnTheSameSession() {
  const CaseScope scope("config reopens after login");
  Rig rig;
  OpenThree(rig);  // config 1, login 2, matchmaker 3
  QCHECK(rig.remotes.Opens().size() == 1);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(1, FirstFrameOf(Role::Config), true);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(3, FirstFrameOf(Role::Matchmaker), true);
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymConfigSuccess, "config"), true);
  rig.router->OnRemoteFrame(session, kUnrequire, true);
  rig.router->OnRemoteFrame(session, LoginReplyFrame(), true);
  rig.router->OnGameClose(1);  // the config socket goes; a socket closing never ends the session
  QCHECK(rig.remotes.Closes().empty());
  QCHECK(rig.router->GetStats().remotes == 1);
  rig.router->OnGameOpen(4);  // the game's new config socket
  QCHECK(rig.remotes.Opens().size() == 1);  // it rides the session
  rig.router->OnGameFrame(4, FirstFrameOf(Role::Config), true);
  QCHECK(rig.remotes.Opens().size() == 1);
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymConfigRequest) == 2);  // both on the one remote
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymConfigSuccess, "config2"), true);   // -> the new config socket
  rig.router->OnRemoteFrame(session, kUnrequire, true);                          // -> the new config socket
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "p"), true);  // -> login
  const std::vector<Delivery> expected = {
      {1, nevr_evr_codec::kSymConfigSuccess},
      {1, nevr_evr_codec::kSymConnectionUnrequire},
      {2, nevr_evr_codec::kSymLoginSuccess},
      {4, nevr_evr_codec::kSymConfigSuccess},
      {4, nevr_evr_codec::kSymConnectionUnrequire},
      {2, nevr_evr_codec::kSymLoggedInUserProfileSuccess},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(rig.remotes.Closes().empty());
  QCHECK(rig.games.Closes().empty());
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
}

// The session ends under the game. Every socket on it closes, the config socket too (it shares the
// session), and when the game reconnects, config included, the sockets share ONE new session.
void TestSessionEndClosesConfigAndConfigReconnectsToANewSession() {
  const CaseScope scope("session end");
  Rig rig;
  OpenThree(rig);
  QCHECK(rig.remotes.Opens().size() == 1);
  const RemoteId first = SessionRemote(rig);
  rig.router->OnRemoteOpen(first);
  rig.router->OnGameFrame(1, FirstFrameOf(Role::Config), true);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnRemoteError(first, 0, "network error");
  std::vector<uint64_t> closed;
  for (const CloseRecord& c : rig.games.Closes()) {
    closed.push_back(c.id);
    QCHECK(c.code == kCloseGoingAway);
  }
  std::sort(closed.begin(), closed.end());
  QCHECK(closed == std::vector<uint64_t>({1, 2, 3}));  // the config socket included
  rig.router->OnGameClose(1);
  rig.router->OnGameClose(2);
  rig.router->OnGameClose(3);
  rig.router->OnGameOpen(4);  // config reconnects
  rig.router->OnGameOpen(5);  // login reconnects
  QCHECK(rig.remotes.Opens().size() == 2);  // one new session remote for both
  const auto opens = rig.remotes.Opens();
  RemoteId second = kNoRemote;
  if (opens.size() == 2) {
    second = opens[1].remote;
    QCHECK(second != first);
    QCHECK(opens[1].session);
  }
  rig.router->OnRemoteOpen(second);
  rig.router->OnGameFrame(4, FirstFrameOf(Role::Config), true);
  rig.router->OnGameFrame(5, FirstFrameOf(Role::Login), true);
  QCHECK(CountUpstream(rig, second, nevr_evr_codec::kSymConfigRequest) == 1);
  QCHECK(CountUpstream(rig, first, nevr_evr_codec::kSymConfigRequest) == 1);  // only the first session's own
  rig.router->OnRemoteFrame(second, Msg(nevr_evr_codec::kSymConfigSuccess, "config"), true);
  rig.router->OnRemoteFrame(second, LoginReplyFrame(), true);
  const std::vector<Delivery> expected = {
      {4, nevr_evr_codec::kSymConfigSuccess},
      {5, nevr_evr_codec::kSymLoginSuccess},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
}

// ---- the config reply owner ----------------------------------------------------------------

// T706 (nakama issue 706): a second matchmaker socket opens while the first still carries the game's find. Lobby frames
// follow the matchmaker socket that last sent a request, not the newest one by order: the game disconnects a
// socket it never asked ("from non-matchmaker peer"). Failure caught: replies going to the newest socket.
void TestLobbyFramesFollowTheMatchmakerThatAsked() {
  const CaseScope scope("lobby follows requester");
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1, Hinted(Role::Config));
  rig.router->OnGameOpen(2, Hinted(Role::Login));
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameOpen(3, Hinted(Role::Matchmaker));
  rig.router->OnGameFrame(3, Msg(kSymSomething, "find"), true);
  rig.router->OnGameOpen(4, Hinted(Role::Matchmaker));  // opened, but sends nothing yet
  rig.router->OnRemoteFrame(session, Msg(kSymSomething, "a"), true);
  auto sent = rig.games.Sent();
  QCHECK(sent.size() == 1 && sent[0].id == 3);
  QCHECK(rig.logs.Has("game=3 conn=2 (matchmaker) sent symbol="));  // the sender of the find is on record
  QCHECK(rig.logs.Has("-> game=3 conn=2 (matchmaker)"));            // and so is where the reply went
  rig.router->OnGameFrame(4, Msg(kSymSomething, "find again"), true);
  rig.router->OnRemoteFrame(session, Msg(kSymSomething, "b"), true);
  sent = rig.games.Sent();
  QCHECK(sent.size() == 2 && sent[1].id == 4);
  rig.router->OnGameFrame(3, Msg(kSymSomething, "find on the first"), true);
  rig.router->OnRemoteFrame(session, Msg(kSymSomething, "c"), true);
  sent = rig.games.Sent();
  QCHECK(sent.size() == 3 && sent[2].id == 3);
}

// The limit of "the matchmaker that last sent a frame" standing in for "the socket whose request this answers":
// both matchmakers are mid-request, so a reply to the earlier asker goes to the later one. Pins that behavior.
void TestLobbyReplyGoesToTheLastAskerWhenTwoAreMidRequest() {
  const CaseScope scope("two askers");
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1, Hinted(Role::Config));
  rig.router->OnGameOpen(2, Hinted(Role::Login));
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameOpen(3, Hinted(Role::Matchmaker));
  rig.router->OnGameOpen(4, Hinted(Role::Matchmaker));
  rig.router->OnGameFrame(3, Msg(kSymSomething, "find"), true);
  rig.router->OnGameFrame(4, Msg(kSymSomething, "find"), true);
  rig.router->OnRemoteFrame(session, Msg(kSymSomething, "answer to 3"), true);
  const auto sent = rig.games.Sent();
  QCHECK(sent.size() == 1 && sent[0].id == 4);
}

// Two config sockets asking for the same resource are answered oldest request first, whichever socket is older.
// Game 3 (a matchmaker by order, then config) asks first, game 1 second; each success, and the Unrequire after
// it, goes to the socket whose request it answers. Failure caught: the oldest socket, or the newest, taking both.
void TestTwoConfigSocketsAskingTheSameResourceAreAnsweredOldestFirst() {
  const CaseScope scope("two config sockets");
  Rig rig;
  OpenThree(rig);  // config 1, login 2, matchmaker 3 (the second config socket, by order)
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(3, ConfigRequestFor("main_menu"), true);  // the newer socket asks first
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);
  QCHECK(rig.remotes.Opens().size() == 1);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // -> 3
  rig.router->OnRemoteFrame(session, kUnrequire, true);                     // -> 3
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // -> 1
  rig.router->OnRemoteFrame(session, kUnrequire, true);                     // -> 1
  const std::vector<Delivery> expected = {
      {3, nevr_evr_codec::kSymConfigSuccess},
      {3, nevr_evr_codec::kSymConnectionUnrequire},
      {1, nevr_evr_codec::kSymConfigSuccess},
      {1, nevr_evr_codec::kSymConnectionUnrequire},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
}

// The service never answers some requests (nakama drops a message over its per-session rate limit with no
// reply, server/session_ws.go). A reply goes to the oldest open request with ITS type and id, so the request
// nobody answered does not shift the replies after it. Game 1's first request is never answered; its second
// is, and the reply is game 1's. Game 1 then closes, a replacement config socket asks for another resource,
// and that reply is the replacement's. Failure caught: replies taken in request order (the unanswered request
// takes the second reply's place, and the replacement's reply goes to the closed socket's stale entry).
void TestAnUnansweredConfigRequestDoesNotShiftLaterReplies() {
  const CaseScope scope("unanswered config request");
  Rig rig;
  rig.router->OnGameOpen(1);  // config
  rig.router->OnGameOpen(2);  // login
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);          // the service drops this one
  rig.router->OnGameFrame(1, ConfigRequestFor("active_store_entry"), true);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("active_store_entry"), true);  // -> 1
  rig.router->OnRemoteFrame(session, kUnrequire, true);                              // -> 1
  rig.router->OnGameClose(1);  // the config socket goes; the game opens a new one
  rig.router->OnGameOpen(3);
  rig.router->OnGameFrame(3, ConfigRequestFor("active_battle_pass_season"), true);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("active_battle_pass_season"), true);  // -> 3
  rig.router->OnRemoteFrame(session, kUnrequire, true);                                     // -> 3
  const std::vector<Delivery> expected = {
      {1, nevr_evr_codec::kSymConfigSuccess},
      {1, nevr_evr_codec::kSymConnectionUnrequire},
      {3, nevr_evr_codec::kSymConfigSuccess},
      {3, nevr_evr_codec::kSymConnectionUnrequire},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 0);
  QCHECK(rig.router->GetStats().configRepliesUnmatched == 0);
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
}

// A failure carries the request's two strings, not symbols: it is matched by them, whatever their case. Game 1
// asks for main_menu, game 3 for a resource the service does not have; the failure for the second goes to
// game 3 although game 1's request is older, and the success for the first goes to game 1.
// Failure caught: a failure delivered in request order, or not matched at all.
void TestAConfigFailureIsMatchedByItsStrings() {
  const CaseScope scope("config failure by strings");
  Rig rig;
  OpenThree(rig);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);
  rig.router->OnGameFrame(3, ConfigRequestFor("Unknown_Type"), true);
  rig.router->OnRemoteFrame(session, ConfigFailureFor("UNKNOWN_TYPE"), true);  // -> 3 (the strings fold to one symbol)
  rig.router->OnRemoteFrame(session, kUnrequire, true);                        // -> 3
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);     // -> 1
  rig.router->OnRemoteFrame(session, kUnrequire, true);                        // -> 1
  const std::vector<Delivery> expected = {
      {3, nevr_evr_codec::kSymConfigFailure},
      {3, nevr_evr_codec::kSymConnectionUnrequire},
      {1, nevr_evr_codec::kSymConfigSuccess},
      {1, nevr_evr_codec::kSymConnectionUnrequire},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
}

// A config reply nobody asked for is dropped, counted, and logged every time with the symbols it carried
// (they are resource names the game asked for, not credentials). Failure caught: handing it to a config
// socket that asked for something else.
void TestAConfigReplyNobodyAskedForIsDroppedAndLogged() {
  const CaseScope scope("unmatched config reply");
  Rig rig;
  OpenThree(rig);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("active_store_entry"), true);
  rig.router->OnRemoteFrame(session, ConfigFailureFor("active_store_entry"), true);
  QCHECK(rig.games.Sent().empty());
  QCHECK(rig.router->GetStats().configRepliesUnmatched == 2);
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 0);
  int lines = 0;
  {
    std::lock_guard<std::mutex> lock(rig.logs.mutex);
    for (const auto& line : rig.logs.lines) {
      if (line.first == LogLevel::Warning && line.second.find("no open config request has its type and id") != std::string::npos &&
          line.second.find("type=0x59db56abec422dc9") != std::string::npos) {
        ++lines;
      }
    }
  }
  QCHECK(lines == 2);  // each drop, not the first and every hundredth
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // the real one still arrives
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 1);
}

// A reply whose requester has closed is dropped and counted, and logged EVERY time even when other quiet
// drops came first (the shared counter used to put the line at the 1st and every 100th drop of any kind).
// Failure caught: an orphaned reply that leaves no line in the log.
void TestAnOrphanedConfigReplyIsLoggedEveryTime() {
  const CaseScope scope("orphan log");
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnRemoteFrame(session, kUnrequire, true);  // an Unrequire nothing is owed: a quiet drop that came first
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);
  rig.router->OnGameFrame(1, ConfigRequestFor("active_store_entry"), true);
  rig.router->OnGameClose(1);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("active_store_entry"), true);
  QCHECK(rig.router->GetStats().droppedUnrequires == 1);
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 2);
  int lines = 0;
  {
    std::lock_guard<std::mutex> lock(rig.logs.mutex);
    for (const auto& line : rig.logs.lines) {
      if (line.first == LogLevel::Warning && line.second.find("the connection that asked (game=1) has closed") != std::string::npos &&
          line.second.find("symbol=0xb9cdaf586f7bd012") != std::string::npos) {
        ++lines;
      }
    }
  }
  QCHECK(lines == 2);
}

// A request the service never answers stays in the list awaiting a reply until the list is full; then the
// oldest is forgotten, counted and logged each time, and a late reply to it is a reply nobody asked for.
// Failure caught: an unbounded list, or a forgotten request that no line mentions.
void TestTheListOfOpenConfigRequestsIsBoundedAndSaysWhenItForgets() {
  const CaseScope scope("config request bound");
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);  // the first of 257, never answered
  for (int i = 0; i < 256; ++i) rig.router->OnGameFrame(1, ConfigRequestFor("active_store_entry"), true);
  QCHECK(rig.router->GetStats().configRequestsEvicted == 1);
  int lines = 0;
  {
    std::lock_guard<std::mutex> lock(rig.logs.mutex);
    for (const auto& line : rig.logs.lines) {
      if (line.first == LogLevel::Warning && line.second.find("config request list full") != std::string::npos) ++lines;
    }
  }
  QCHECK(lines == 1);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // the forgotten request's reply
  QCHECK(rig.games.Sent().empty());
  QCHECK(rig.router->GetStats().configRepliesUnmatched == 1);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("active_store_entry"), true);  // a remembered one
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 1);
}

// A config socket closes with its request in flight and a new one takes its place and asks for the same resource.
// The oldest open request for the reply's key is the closed socket's, but a reply to a resource goes to a socket
// that asked for it, so the replacement's open request takes the reply and both entries go. If the closed socket's
// reply was really in flight, the replacement has what it asked for, and the reply to its own request, which
// finds no open request, is dropped and counted as unmatched. The Unrequires stay consistent: the first reply's
// goes to the replacement (its count 1 -> 0), the second reply's is owed to nobody and is dropped.
// Failure caught: the replacement starved by a stale entry (the reply taken and dropped), or a reply delivered twice.
void TestAClosedConfigSocketsStaleEntryYieldsToItsReplacement() {
  const CaseScope scope("stale entry yields");
  Rig rig;
  OpenThree(rig);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);  // in flight, or never answered
  rig.router->OnGameClose(1);
  rig.router->OnGameOpen(4);
  rig.router->OnGameFrame(4, ConfigRequestFor("main_menu"), true);  // the replacement's request, behind it
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // -> 4, both entries go
  rig.router->OnRemoteFrame(session, kUnrequire, true);                      // -> 4 (count 1 -> 0)
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // the reply to 4's own request: dropped
  rig.router->OnRemoteFrame(session, kUnrequire, true);                      // owed to nobody: dropped
  const std::vector<Delivery> expected = {
      {4, nevr_evr_codec::kSymConfigSuccess},
      {4, nevr_evr_codec::kSymConnectionUnrequire},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 0);
  QCHECK(rig.router->GetStats().configRepliesUnmatched == 1);
  QCHECK(rig.router->GetStats().droppedUnrequires == 1);
  QCHECK(rig.logs.Has("no open config request has its type and id"));
  // The hand-over is logged at Info and names the socket that took the reply and the closed one it was owed to.
  QCHECK(rig.logs.Has("goes to game=4: the oldest open request for it, game=1, has closed", static_cast<int>(LogLevel::Info)));
}

// The stale-entry hazard, measured over replacements: a config request the service never answers whose socket
// closes leaves an open entry. Five replacement sockets each ask for the same resource once; the service answers
// each one's request. Every one of them gets its reply (before the rule above, none did). Failure caught: a
// stale entry starving every replacement that asks the same (type, id) until the session ends.
void TestAStaleConfigEntryDoesNotStarveReplacementSockets() {
  const CaseScope scope("stale entry rounds");
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);  // the service drops this one
  rig.router->OnGameClose(1);
  GameId next = 3;
  for (int round = 1; round <= 5; ++round) {
    const GameId g = next++;
    rig.router->OnGameOpen(g);
    rig.router->OnGameFrame(g, ConfigRequestFor("main_menu"), true);
    rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // answers this socket's request
    int replies = 0;
    for (const Delivery& d : Delivered(rig)) replies += (d.first == g && d.second == nevr_evr_codec::kSymConfigSuccess) ? 1 : 0;
    QCHECK(replies == 1);
    rig.router->OnGameClose(g);
  }
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 0);
  QCHECK(rig.router->GetStats().configRepliesUnmatched == 0);
}

// Two requests for the same resource on one replacement socket: each reply is its own. The first reply takes the
// stale entry's place, the second is the replacement's second request.
void TestTwoRequestsOnAReplacementSocketBehindAStaleEntryAreBothAnswered() {
  const CaseScope scope("stale entry two requests");
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);
  rig.router->OnGameClose(1);
  rig.router->OnGameOpen(3);
  rig.router->OnGameFrame(3, ConfigRequestFor("main_menu"), true);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);
  rig.router->OnGameFrame(3, ConfigRequestFor("main_menu"), true);  // the same socket asks again
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);
  int replies = 0;
  for (const Delivery& d : Delivered(rig)) replies += (d.first == 3 && d.second == nevr_evr_codec::kSymConfigSuccess) ? 1 : 0;
  QCHECK(replies == 2);
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 0);
}

// Several closed sockets' stale entries for one key sit ahead of a live socket's request. The reply goes to the
// live socket (the stale ones are skipped, only the first is consumed), and the next reply for that key, which
// finds the second stale entry and no live asker, is an orphan.
void TestStaleEntriesAreSkippedToReachALiveRequest() {
  const CaseScope scope("stale entries skipped");
  Rig rig;
  OpenThree(rig);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);
  rig.router->OnGameOpen(5);
  rig.router->OnGameFrame(5, ConfigRequestFor("main_menu"), true);
  rig.router->OnGameClose(1);
  rig.router->OnGameClose(5);
  rig.router->OnGameOpen(6);
  rig.router->OnGameFrame(6, ConfigRequestFor("main_menu"), true);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // -> 6; game 1's entry and 6's go
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 6);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // game 5's entry, nobody live asked
  QCHECK(rig.games.Sent().size() == 1);
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 1);
}

// The stale entry is not the last one and the live entry is not either: the list holds seven entries when the
// reply arrives, and the stale entry has an older open request ahead of it (one the service never answered, from a
// socket that is still open). The hand-over erases two entries of the list. Failure caught: the second erase
// reaching the wrong entry, so the older socket's open request is the one dropped (its reply then finds nothing
// and is dropped as unmatched) and the stale entry survives to orphan the next reply for its key.
void TestAHandOverBehindAnOlderOpenRequestErasesTheStaleEntryAndTheLiveOne() {
  const CaseScope scope("stale entry behind an older request");
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameOpen(9);
  rig.router->OnGameFrame(9, ConfigRequestFor("x_resource"), true);     // older, open, another resource
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);      // the stale one after the close
  rig.router->OnGameClose(1);
  rig.router->OnGameOpen(3);
  rig.router->OnGameFrame(3, ConfigRequestFor("main_menu"), true);
  rig.router->OnGameFrame(3, ConfigRequestFor("active_battle_pass_season"), true);
  rig.router->OnGameFrame(3, ConfigRequestFor("active_store_entry"), true);
  rig.router->OnGameFrame(3, ConfigRequestFor("active_store_featured_entry"), true);
  rig.router->OnGameFrame(3, ConfigRequestFor("extra_one"), true);
  // Entries: [9:x_resource, 1:main_menu (closed), 3:main_menu, 3:bp, 3:store, 3:featured, 3:extra].
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);   // -> 3; entries 1 and 3's main_menu go
  rig.router->OnRemoteFrame(session, kUnrequire, true);                      // -> 3
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("x_resource"), true);  // -> 9, whose request is still open
  rig.router->OnRemoteFrame(session, kUnrequire, true);                      // -> 9
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);   // nobody has a main_menu request open
  rig.router->OnRemoteFrame(session, kUnrequire, true);                      // owed to nobody: dropped
  const std::vector<Delivery> expected = {
      {3, nevr_evr_codec::kSymConfigSuccess},
      {3, nevr_evr_codec::kSymConnectionUnrequire},
      {9, nevr_evr_codec::kSymConfigSuccess},
      {9, nevr_evr_codec::kSymConnectionUnrequire},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 0);
  QCHECK(rig.router->GetStats().configRepliesUnmatched == 1);
  QCHECK(rig.router->GetStats().droppedUnrequires == 1);
}

// The shape a reopened config socket produces: the stale entry, then the replacement's request for the same
// resource, then its requests for the other three. The live entry has entries after it, and every one of the
// four replies and its Unrequire reaches the replacement. Failure caught: a hand-over that erases through an
// iterator the first erase invalidated (undefined behaviour; the standard library's checked mode aborts on it).
void TestAHandOverWithEntriesAfterTheLiveOneAnswersEveryResource() {
  const CaseScope scope("stale entry then four resources");
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);  // never answered
  rig.router->OnGameClose(1);
  rig.router->OnGameOpen(3);
  const char* const names[] = {"main_menu", "active_battle_pass_season", "active_store_entry", "active_store_featured_entry"};
  for (const char* name : names) rig.router->OnGameFrame(3, ConfigRequestFor(name), true);
  for (const char* name : names) {
    rig.router->OnRemoteFrame(session, ConfigSuccessFor(name), true);
    rig.router->OnRemoteFrame(session, kUnrequire, true);
  }
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == 8);
  for (std::size_t i = 0; i < got.size(); ++i) {
    QCHECK(got[i].first == 3);
    QCHECK(got[i].second == (i % 2 == 0 ? nevr_evr_codec::kSymConfigSuccess : nevr_evr_codec::kSymConnectionUnrequire));
  }
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 0);
  QCHECK(rig.router->GetStats().configRepliesUnmatched == 0);
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
}

// A session end forgets the open config requests: they were the lost session's, and the service of the next
// session never saw them. A reply for the same resource on the new session, which nobody asked for there, is
// unmatched, not a closed socket's orphan. Failure caught: requests of an ended session kept in the list.
void TestASessionEndForgetsTheOpenConfigRequests() {
  const CaseScope scope("session end forgets requests");
  Rig rig;
  OpenThree(rig);
  const RemoteId first = SessionRemote(rig);
  rig.router->OnRemoteOpen(first);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);
  rig.router->OnRemoteError(first, 0, "network error");
  rig.router->OnGameClose(1);
  rig.router->OnGameClose(2);
  rig.router->OnGameClose(3);
  rig.router->OnGameOpen(4);
  rig.router->OnGameOpen(5);
  const RemoteId second = SessionRemote(rig);
  QCHECK(second != first);
  rig.router->OnRemoteOpen(second);
  rig.router->OnRemoteFrame(second, ConfigSuccessFor("main_menu"), true);
  QCHECK(rig.games.Sent().empty());
  QCHECK(rig.router->GetStats().configRepliesUnmatched == 1);
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 0);
}

// When no live socket has an open request for the key, the closed socket's reply is dropped and counted as
// orphaned (there is nothing to hand it to), and the line says so once, without the generic drop line.
void TestAClosedSocketsReplyIsDroppedWhenNoLiveSocketAskedForIt() {
  const CaseScope scope("orphan without a live asker");
  Rig rig;
  OpenThree(rig);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);
  rig.router->OnGameClose(1);
  rig.router->OnGameOpen(4);
  rig.router->OnGameFrame(4, ConfigRequestFor("active_store_entry"), true);  // another resource
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);
  QCHECK(rig.games.Sent().empty());
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 1);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("active_store_entry"), true);  // game 4's own is unharmed
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 4);
}

// A config reply dropped here is logged by the line that names it and not also by the generic "dropped on
// purpose" line, whose total excludes config drops (with no other drops it would fire every time).
void TestAConfigDropWritesOnlyItsOwnLine() {
  const CaseScope scope("config drop one line");
  Rig rig;
  OpenThree(rig);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // nobody asked: unmatched
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu"), true);
  rig.router->OnGameClose(1);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu"), true);  // the asker closed: orphaned
  int own = 0, generic = 0;
  {
    std::lock_guard<std::mutex> lock(rig.logs.mutex);
    for (const auto& line : rig.logs.lines) {
      if (line.second.find("config reply dropped") != std::string::npos) ++own;
      if (line.second.find("dropped on purpose") != std::string::npos) ++generic;
    }
  }
  QCHECK(own == 2);
  QCHECK(generic == 0);
}

// The key is the type AND the id: two open requests of one type with different ids are told apart. Game 1 asks
// for ("main_menu", "a"), game 3 for ("main_menu", "b"); the reply for "b" is game 3's although game 1's request
// is older and has the same type. Failure caught: replies matched by type alone.
void TestRequestsOfOneTypeWithDifferentIdsAreToldApart() {
  const CaseScope scope("same type different id");
  Rig rig;
  OpenThree(rig);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(1, ConfigRequestFor("main_menu", "a"), true);
  rig.router->OnGameFrame(3, ConfigRequestFor("main_menu", "b"), true);
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu", "b"), true);  // -> 3
  rig.router->OnRemoteFrame(session, ConfigSuccessFor("main_menu", "a"), true);  // -> 1
  const std::vector<Delivery> expected = {
      {3, nevr_evr_codec::kSymConfigSuccess},
      {1, nevr_evr_codec::kSymConfigSuccess},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
}

// SNSConfigFailurev2 is a config reply: it goes to the config socket, and its Unrequire after it.
// Failure caught: the failure falling to the matchmaker (lobby traffic) or the login socket.
void TestConfigFailureGoesToTheConfigSocket() {
  const CaseScope scope("config failure");
  Rig rig;
  OpenThree(rig);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  rig.router->OnGameFrame(3, FirstFrameOf(Role::Matchmaker), true);
  rig.router->OnGameFrame(1, FirstFrameOf(Role::Config), true);
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymConfigFailure, "no"), true);  // -> config
  rig.router->OnRemoteFrame(session, kUnrequire, true);                                    // -> config
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "p"), true);
  const std::vector<Delivery> expected = {
      {1, nevr_evr_codec::kSymConfigFailure},
      {1, nevr_evr_codec::kSymConnectionUnrequire},
      {2, nevr_evr_codec::kSymLoggedInUserProfileSuccess},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
}

// ---- a held session ---------------------------------------------------------------------------

// The login gate holds the login connection's session; the config connection before it was refused at once.
// The login's request queues, and on Ready the session opens, the request is flushed and its reply arrives.
// Failure caught: the held session never opening, or a request leaving before the remote is up.
void TestHeldSessionQueuesLoginThenFlushesOnReady() {
  const CaseScope scope("held session");
  Gate gate;
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.remotes.openResult = false;
  rig.router->OnGameOpen(1);  // config: refused at once
  rig.remotes.openResult = true;
  rig.router->OnGameOpen(2);  // login
  QCHECK(rig.remotes.Opens().size() == 1);  // only config's refused attempt while the account is awaited
  QCHECK(rig.router->GetStats().heldRemotes == 1);
  QCHECK(Exempt(rig, 2));
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  QCHECK(rig.router->GetStats().pendingFrames == 1);  // queued, in the held session
  QCHECK(rig.remotes.Sent().empty());
  gate.Set(LoginGate::Ready);
  rig.router->ReevaluateHeldLogins();
  QCHECK(rig.remotes.Opens().size() == 2);
  QCHECK(rig.remotes.Opens().size() < 2 || rig.remotes.Opens()[1].session);
  QCHECK(rig.router->GetStats().heldRemotes == 0);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 1);  // flushed
  QCHECK(rig.router->GetStats().pendingFrames == 0);
}

// G4 (#709): a config connection opened the session and the transport accepted it while the account is
// awaited; the login connection then attaches to an OPEN session. No login is injected until the gate says
// Ready; Ready injects it once. Failure caught: the hold being bypassed by a session the config socket opened.
void TestGateDefersLoginInjectionOnAnAlreadyOpenSession() {
  const CaseScope scope("gate defers injection");
  Gate gate;
  Options o = WithLogin(kLogin);
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1, Hinted(Role::Config));
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameOpen(2, Hinted(Role::Login));
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 0);
  rig.router->OnGameOpen(3, Hinted(Role::Matchmaker));
  rig.router->OnGameFrame(3, Msg(kSymSomething, "find"), true);
  QCHECK(CountUpstream(rig, session, kSymSomething) == 0);  // behind the login that is waiting
  gate.Set(LoginGate::Ready);
  rig.router->ReevaluateHeldLogins();
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 1);
  QCHECK(CountUpstream(rig, session, kSymSomething) == 1);
  rig.router->ReevaluateHeldLogins();
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 1);
}

// G4: the gate ending in Refused ends the session instead of leaving the login waiting forever.
void TestGateRefusedEndsADeferredInjection() {
  const CaseScope scope("gate refused after defer");
  Gate gate;
  Options o = WithLogin(kLogin);
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1, Hinted(Role::Config));
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameOpen(2, Hinted(Role::Login));
  gate.Set(LoginGate::Refused);
  rig.router->ReevaluateHeldLogins();
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 0);
  QCHECK(!rig.games.Closes().empty());
}

// G4: the gate turns Awaiting between the login opening the session and the transport reporting it open. The
// session opens for the config pass, the login waits, and Ready injects it once.
void TestGateAwaitingWhenTheSessionOpensDefersTheLogin() {
  const CaseScope scope("gate awaiting at open");
  Gate gate;
  gate.Set(LoginGate::Ready);
  Options o = WithLogin(kLogin);
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1, Hinted(Role::Login));
  const RemoteId session = SessionRemote(rig);
  gate.Set(LoginGate::Awaiting);
  rig.router->OnRemoteOpen(session);
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 0);
  gate.Set(LoginGate::Ready);
  rig.router->ReevaluateHeldLogins();
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 1);
}

// G1: a frame the runtime makes (a party update) goes on the session remote like a game frame: queued behind
// the login while the session is not open, sent in order once it is, and refused when there is no session.
void TestSendToSessionQueuesBehindTheLoginAndThenSends() {
  const CaseScope scope("send to session");
  Rig rig(WithLogin(kLogin));
  QCHECK(!rig.router->SendToSession(Msg(kSymSomething, "no session")));
  rig.router->OnGameOpen(1, Hinted(Role::Login));
  const RemoteId session = SessionRemote(rig);
  QCHECK(rig.router->SendToSession(Msg(kSymSomething, "early")));  // the session is not open: queued
  QCHECK(CountUpstream(rig, session, kSymSomething) == 0);
  rig.router->OnRemoteOpen(session);
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 1);
  QCHECK(CountUpstream(rig, session, kSymSomething) == 1);  // behind the login
  QCHECK(rig.router->SendToSession(Msg(kSymSomething, "late")));
  QCHECK(CountUpstream(rig, session, kSymSomething) == 2);
}

// G3: a dedicated server's LoginFailure is replaced by the frame the option returns; the game gets it as a
// LoginSuccess, the log says it was synthesized, and no friend-list subscribe follows. Without the option the
// failure reaches the game unchanged.
void TestLoginFailureReplacementIsDeliveredAndNotSubscribed() {
  const CaseScope scope("login failure replacement");
  Options o = WithLogin(kLogin);
  o.subscribeFriendList = true;
  o.replaceLoginFailure = []() { return std::optional<std::string>(Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0'))); };
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymLoginFailure, std::string(24, '\0')), true);
  const auto sent = rig.games.Sent();
  QCHECK(sent.size() == 1 && nevr_evr_codec::FirstSymbol(sent[0].data) == nevr_evr_codec::kSymLoginSuccess);
  QCHECK(sent.empty() || sent[0].id == 2);
  QCHECK(rig.logs.Has("synthesized"));
  QCHECK(!rig.logs.Has("LOGIN SUCCESS"));
  QCHECK(rig.remotes.Sent().size() == 1);  // the login only: no subscribe
}

// S2b: the adapter learns when the SESSION ends (not when a standalone matchmaker's remote does), once, after
// the router's lock is released, and can ask which remote is the session.
void TestSessionEndedCallbackFiresOncePerSession() {
  const CaseScope scope("session ended callback");
  int ended = 0;
  Options o = WithLogin(kLogin);
  o.onSessionEnded = [&ended]() { ++ended; };
  Rig rig(std::move(o));
  QCHECK(!rig.router->SessionRemote().has_value());
  rig.router->OnGameOpen(1, Hinted(Role::Matchmaker));  // standalone remote: not the session
  rig.router->OnGameOpen(2, Hinted(Role::Login));
  const RemoteId session = SessionRemote(rig);
  QCHECK(rig.router->SessionRemote().has_value() && *rig.router->SessionRemote() == session);
  const auto opens = rig.remotes.Opens();
  QCHECK(opens.size() == 2);
  if (opens.size() != 2) return;
  rig.router->OnRemoteClose(opens[0].remote, 1000);  // the standalone matchmaker's remote
  QCHECK(ended == 0);
  rig.router->OnRemoteClose(session, 1006);
  QCHECK(ended == 1);
  QCHECK(!rig.router->SessionRemote().has_value());
  rig.router->OnRemoteClose(session, 1006);  // already gone
  QCHECK(ended == 1);
}

// a held session whose login socket closes and reopens three times. A closing socket's queued frames
// leave the session's queue with it, so Ready flushes one login per LIVE socket, and the counts stay
// consistent. Failure caught: the closed sockets' requests flushed to the service, whose replies would be
// orphans.
void TestHeldSessionPurgesAClosedLoginSocketsQueuedFrames() {
  const CaseScope scope("held session purge");
  Gate gate;
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.remotes.openResult = false;
  rig.router->OnGameOpen(1);  // config: refused at once
  rig.remotes.openResult = true;
  rig.router->OnGameOpen(2);  // login, first life
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Login), true);
  QCHECK(rig.router->GetStats().pendingFrames == 1);
  // The login socket closes three times and reopens each time: lives 2, 3, 4 close, lives 3, 4, 5 open.
  const GameId lives[] = {2, 3, 4, 5};
  for (std::size_t i = 0; i + 1 < sizeof(lives) / sizeof(lives[0]); ++i) {
    rig.router->OnGameClose(lives[i]);
    QCHECK(rig.router->GetStats().pendingFrames == 0);  // the closed socket's request left with it
    rig.router->OnGameOpen(lives[i + 1]);
    rig.router->OnGameFrame(lives[i + 1], FirstFrameOf(Role::Login), true);
    QCHECK(rig.router->GetStats().pendingFrames == 1);
  }
  QCHECK(rig.router->GetStats().heldRemotes == 1);
  QCHECK(rig.remotes.Opens().size() == 1);  // config's refused attempt only
  gate.Set(LoginGate::Ready);
  rig.router->ReevaluateHeldLogins();
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 1);  // game 5's, nobody else's
  QCHECK(rig.router->GetStats().pendingFrames == 0);
  QCHECK(rig.router->GetStats().droppedUnrequires == 0);
  QCHECK(rig.router->GetStats().outboundBytes == 0);
  QCHECK(rig.router->GetStats().configRepliesOrphaned == 0);
}

// the matchmaker cap counts a connection by its hint. Nine hinted matchmakers open before any
// config or login socket: the router's order-based guess would call the first two config and login, so
// only seven would count. The ninth is refused with the cap's close code and the other eight stay.
void TestTheNinthHintedMatchmakerIsRefused() {
  const CaseScope scope("ninth hinted matchmaker");
  Rig rig;
  for (GameId g = 1; g <= 9; ++g) rig.router->OnGameOpen(g, Hinted(Role::Matchmaker));
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1);
  if (closes.size() == 1) {
    QCHECK(closes[0].id == 9);
    QCHECK(closes[0].code == kCloseTryAgainLater);
  }
  QCHECK(rig.router->GetStats().games == 8);
}

// a connection relabelled to matchmaker by its first frame is counted against the cap like one that
// opened as a matchmaker. With the cap at 1 and a matchmaker already on the session, the provisional login
// whose first frame is a lobby request is refused (closed with the cap's code) and its frame goes nowhere.
// Failure caught: the cap bypassed by opening as login and relabelling.
void TestRelabelToMatchmakerChecksTheCap() {
  const CaseScope scope("relabel cap");
  Options o;
  o.limits.maxMatchmakerConnections = 1;
  Rig rig(std::move(o));
  OpenThree(rig);  // config 1, login 2, matchmaker 3: one matchmaker, at the cap
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  QCHECK(rig.games.Closes().empty());
  rig.router->OnGameFrame(2, FirstFrameOf(Role::Matchmaker), true);  // the provisional login is a matchmaker
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1);
  if (closes.size() == 1) {
    QCHECK(closes[0].id == 2);
    QCHECK(closes[0].code == kCloseTryAgainLater);
  }
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymFindSessionRequest) == 0);  // the frame is not forwarded
  QCHECK(rig.router->GetStats().droppedGameFrames == 1);
}

// The previous login connection rides the session as a matchmaker when another connection takes the login
// over. Game 3 (a matchmaker by order) names itself the login with its first frame; game 2, the old login,
// is then the only matchmaker, so lobby traffic goes to it and not to the new login connection.
// Failure caught: the old login keeping its login role (lobby traffic then falls to the new login).
void TestTheDemotedLoginConnectionRidesTheSessionAsAMatchmaker() {
  const CaseScope scope("demoted login");
  Rig rig;
  rig.router->OnGameOpen(1);  // config
  rig.router->OnGameOpen(2);  // login
  rig.router->OnGameOpen(3);  // a matchmaker by order
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(3, FirstFrameOf(Role::Login), true);  // takes the login over
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);  // -> 3
  rig.router->OnRemoteFrame(session, Msg(kSymLobbySessionSuccess, "lobby"), true);                          // -> 2
  const std::vector<Delivery> expected = {
      {3, nevr_evr_codec::kSymLoginSuccess},
      {2, kSymLobbySessionSuccess},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(!Exempt(rig, 2));
  QCHECK(Exempt(rig, 3));
}

// The login is injected into an OPEN session when a login connection attaches, and the frames the game sends
// while the builder runs wait behind it, except a config connection's, which pass. The builder here sends one
// of each from inside itself (it runs with no lock held, so it may re-enter the router). The session sees the
// config frame, then the login, then the login connection's own frame.
// Failure caught: a non-config frame overtaking the login; a config frame held behind it.
void TestFramesWaitBehindTheLoginBeingBuiltExceptConfigs() {
  const CaseScope scope("behind the login");
  Rig* rigPtr = nullptr;
  const std::string configFrame = Msg(nevr_evr_codec::kSymConfigRequest, "cfg-during-build");
  const std::string loginFrame = Msg(nevr_evr_codec::kSymLoggedInUserProfileRequest, "profile-during-build");
  Options o;
  o.buildLogin = [&]() {
    rigPtr->router->OnGameFrame(1, configFrame, true);
    rigPtr->router->OnGameFrame(2, loginFrame, true);
    return std::optional<std::string>(kLogin);
  };
  Rig rig(std::move(o));
  rigPtr = &rig;
  rig.router->OnGameOpen(1, Hinted(Role::Config));  // opens the session
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);  // open, with no login connection attached: nothing is built
  QCHECK(rig.remotes.Sent().empty());
  rig.router->OnGameOpen(2, Hinted(Role::Login));  // attaches to the open session: the login is built now
  const std::vector<SentFrame> up = rig.remotes.Sent();
  QCHECK(up.size() == 3);
  if (up.size() == 3) {
    QCHECK(up[0].data == configFrame);
    QCHECK(up[1].data == kLogin);
    QCHECK(up[2].data == loginFrame);
  }
}

// The pending-bytes limit counts only the frames still queued. A socket that closes takes its queued bytes
// with it, so a replacement can queue as much as the limit allows.
// Failure caught: the closed socket's bytes staying counted, the replacement closed 1013 for a queue that is
// not full.
void TestAClosedSocketsQueuedBytesDoNotCountAgainstTheLimit() {
  const CaseScope scope("pending bytes");
  Options o;
  o.limits.maxPendingBytes = 100;
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameFrame(1, std::string(60, 'a'), true);  // queued: the session is not open
  QCHECK(rig.router->GetStats().pendingFrames == 1);
  rig.router->OnGameClose(1);
  QCHECK(rig.router->GetStats().pendingFrames == 0);
  rig.router->OnGameOpen(2);
  rig.router->OnGameFrame(2, std::string(60, 'b'), true);  // 60 <= 100 once game 1's 60 bytes are gone
  QCHECK(rig.games.Closes().empty());
  QCHECK(rig.router->GetStats().pendingFrames == 1);
}

// The matchmaker cap counts a connection by its role, which a hint sets. Eight hinted matchmakers are at the
// cap, and a hinted config socket that arrives next is not a matchmaker: it is accepted, though the order
// alone would call it one.
// Failure caught: the cap applied to the order-based guess, refusing the config socket.
void TestAHintedConfigSocketIsNotRefusedByTheMatchmakerCap() {
  const CaseScope scope("cap by hint");
  Rig rig;
  for (GameId g = 1; g <= 8; ++g) rig.router->OnGameOpen(g, Hinted(Role::Matchmaker));
  QCHECK(rig.games.Closes().empty());
  rig.router->OnGameOpen(9, Hinted(Role::Config));
  QCHECK(rig.games.Closes().empty());
  QCHECK(rig.router->GetStats().games == 9);
}

// ---- config reply matching keys (vectors from nakama's own code) -------------------------------------
//
// The expected values below were produced by running the service's functions unchanged: ToSymbol
// (server/evr/core_packet.go with CalculateSymbolValue, core_hash.go) and encoding/json decoding of the
// ConfigRequest {type, id} and ConfigErrorInfo {type, identifier} structs (server/evr/config_request.go,
// config_failure.go), in a scratch Go module that holds verbatim copies of those functions; the nakama
// repository was only read. ok is whether encoding/json decoded the text without error. The values for names with a
// byte >= 0x80 follow the service's sign extension (nakama#711): they are the game's own EchoVR::CSymbol64Hash of
// the bytes the decoder emits, which the nakama#710 vectors pin to the service.

struct SymbolVector {
  std::string text;
  uint64_t symbol;
};
const SymbolVector kSymbolVectors[] = {
    {std::string("", 0), 0x0000000000000000ULL},
    {std::string("main_menu", 9), 0x1509ee27440e1d7bULL},
    {std::string("Main_Menu", 9), 0x1509ee27440e1d7bULL},
    {std::string("MAIN_MENU", 9), 0x1509ee27440e1d7bULL},
    {std::string("active_battle_pass_season", 25), 0x794e13955045c77eULL},
    {std::string("active_store_entry", 18), 0x59db56abec422dc9ULL},
    {std::string("active_store_featured_entry", 27), 0x554922b112d2f931ULL},
    {std::string("x", 1), 0xd1d52cf94503a85eULL},
    {std::string("unknown_type", 12), 0xd1ad72f8234f41f9ULL},
    {std::string("0x82869f0b37eb4378", 18), 0x82869f0b37eb4378ULL},
    {std::string("0x0000000000000000", 18), 0x0000000000000000ULL},
    {std::string("0xFFFFFFFFFFFFFFFF", 18), 0xffffffffffffffffULL},
    {std::string("0xabcdef0123456789", 18), 0xabcdef0123456789ULL},
    {std::string("0XABCDEF0123456789", 18), 0xf0e2d8cdf22ca74dULL},
    {std::string("0xZZZZZZZZZZZZZZZZ", 18), 0x0f14c3a68c0894eaULL},
    {std::string("0x82869f0b37eb437", 17), 0x4939a90bbe8e6997ULL},
    {std::string("0x82869f0b37eb43780", 19), 0xc8950adf7338f1bcULL},
    {std::string("0x", 2), 0x313d0759bc6bdc32ULL},
    {std::string("0x1", 3), 0xc8e8d0b1a8c0fabbULL},
    {std::string("caf\303\251", 5), 0xcd0714df62754f5fULL},   // bytes >= 0x80 are sign-extended (nakama#710, #908)
    {std::string("\377\376", 2), 0x1f17d45f0697bb92ULL},
    {std::string("main menu", 9), 0x1509ee58440e1d7bULL},
    {std::string("type\000id", 7), 0x2fc3968f5b145e8eULL},
    {std::string("0x+123456789abcdef", 18), 0x62ba849bc1ce5820ULL},
    {std::string("0x_123456789abcdef", 18), 0xba7effc0e5b634e8ULL},
};

struct KeyVector {
  std::string json;
  bool ok;
  uint64_t type;
  uint64_t id;
};
const KeyVector kRequestVectors[] = {
    {std::string("{\"type\":\"main_menu\",\"id\":\"main_menu\"}", 37), true, 0x1509ee27440e1d7bULL, 0x1509ee27440e1d7bULL},
    {std::string("{\"id\":\"b\",\"type\":\"a\"}", 21), true, 0xd1d52cf94503a847ULL, 0xd1d52cf94503a844ULL},
    {std::string("{\"type\":\"Main_Menu\",\"id\":\"MAIN_MENU\"}", 37), true, 0x1509ee27440e1d7bULL, 0x1509ee27440e1d7bULL},
    {std::string("{\"type\":\"a\\\"b\",\"id\":\"c\\\\d\"}", 27), true, 0xc8e8d0b1a891a0e8ULL, 0xc8e8d0b1a893deeeULL},
    {std::string("{\"type\":\"Abc\",\"id\":\"\303\251\"}", 24), true, 0xc8e8d0b1a891e0e9ULL, 0x1f17d45f069787c5ULL},
    {std::string("{\"type\":\"x\\/y\",\"id\":\"\\n\"}", 25), true, 0xc8e8d0b1a888adf3ULL, 0xd1d52cf94503a82cULL},
    {std::string("{\"type\":\"a\",\"type\":\"b\",\"id\":\"c\"}", 32), true, 0xd1d52cf94503a844ULL, 0xd1d52cf94503a845ULL},
    {std::string("{\"Type\":\"a\",\"ID\":\"b\"}", 21), true, 0xd1d52cf94503a847ULL, 0xd1d52cf94503a844ULL},
    {std::string("{\"TYPE\":\"a\",\"Id\":\"b\"}", 21), true, 0xd1d52cf94503a847ULL, 0xd1d52cf94503a844ULL},
    {std::string("{\"type\":\"a\"}", 12), true, 0xd1d52cf94503a847ULL, 0x0000000000000000ULL},
    {std::string("{\"type\":5,\"id\":\"b\"}", 19), false, 0x0000000000000000ULL, 0xd1d52cf94503a844ULL},
    {std::string("{\"type\":null,\"id\":\"b\"}", 22), true, 0x0000000000000000ULL, 0xd1d52cf94503a844ULL},
    {std::string("{\"type\":\"a\",\"type\":null,\"id\":\"b\"}", 33), true, 0xd1d52cf94503a847ULL, 0xd1d52cf94503a844ULL},
    {std::string("{\"type\":\"a\",\"id\":\"b\",\"extra\":{\"type\":\"z\",\"list\":[1,2,{\"id\":\"q\"}]}}", 66), true, 0xd1d52cf94503a847ULL, 0xd1d52cf94503a844ULL},
    {std::string("{\"type\":\"\360\237\230\200\",\"id\":\"s\"}", 24), true, 0x1f3c3aa6fb1420b6ULL, 0xd1d52cf94503a855ULL},
    {std::string("{\"type\":\"\\ud83d\",\"id\":\"s\"}", 26), true, 0xd7ff04eeae77795bULL, 0xd1d52cf94503a855ULL},
    {std::string("{\"type\":\"\\ude00\",\"id\":\"s\"}", 26), true, 0xd7ff04eeae77795bULL, 0xd1d52cf94503a855ULL},
    {std::string("{\"type\":\"\\ud83dx\",\"id\":\"s\"}", 27), true, 0xe0c3c5591bcbfa4eULL, 0xd1d52cf94503a855ULL},
    {std::string("{\"type\":\"a\377b\",\"id\":\"x\"}", 23), true, 0xcb2d3ccaf837a054ULL, 0xd1d52cf94503a85eULL},
    {std::string("{\"type\":\"caf\303\251\",\"id\":\"x\"}", 25), true, 0xcd0714df62754f5fULL, 0xd1d52cf94503a85eULL},
    {std::string("{\"type\":\"a\\u0000b\",\"id\":\"x\"}", 28), true, 0xc8e8d0b1a89182e8ULL, 0xd1d52cf94503a85eULL},
    {std::string("{\"type\":\"0x82869f0b37eb4378\",\"id\":\"0x0000000000000001\"}", 55), true, 0x82869f0b37eb4378ULL, 0x0000000000000001ULL},
    {std::string("{\"type\":\"\",\"id\":\"\"}", 19), true, 0x0000000000000000ULL, 0x0000000000000000ULL},
    {std::string("{}", 2), true, 0x0000000000000000ULL, 0x0000000000000000ULL},
    {std::string("  {  \"type\" : \"a\" ,  \"id\" : \"b\"  }  ", 36), true, 0xd1d52cf94503a847ULL, 0xd1d52cf94503a844ULL},
    {std::string("not json", 8), false, 0x0000000000000000ULL, 0x0000000000000000ULL},
    {std::string("{\"type\":\"a\",", 12), false, 0x0000000000000000ULL, 0x0000000000000000ULL},
    {std::string("{\"type\":\"a\\q\",\"id\":\"b\"}", 23), false, 0x0000000000000000ULL, 0x0000000000000000ULL},
    {std::string("{\"type\":\"a\012b\",\"id\":\"b\"}", 23), false, 0x0000000000000000ULL, 0x0000000000000000ULL},
    {std::string("{\"id\":\"main_menu\",\"type\":\"main_menu\"} ", 38), true, 0x1509ee27440e1d7bULL, 0x1509ee27440e1d7bULL},
};
const KeyVector kFailureVectors[] = {
    {std::string("{\"type\":\"main_menu\",\"identifier\":\"main_menu\",\"errorCode\":1,\"error\":\"resource not found\"}", 88), true, 0x1509ee27440e1d7bULL, 0x1509ee27440e1d7bULL},
    {std::string("{\"identifier\":\"b\",\"type\":\"a\"}", 29), true, 0xd1d52cf94503a847ULL, 0xd1d52cf94503a844ULL},
    {std::string("{\"type\":\"A\",\"identifier\":\"B\"}", 29), true, 0xd1d52cf94503a847ULL, 0xd1d52cf94503a844ULL},
    {std::string("{\"type\":\"x\"}", 12), true, 0xd1d52cf94503a85eULL, 0x0000000000000000ULL},
    {std::string("{\"type\":3,\"identifier\":\"b\"}", 27), false, 0x0000000000000000ULL, 0xd1d52cf94503a844ULL},
};

// ConfigSymbol is the service's ToSymbol: empty is 0, an 18-character "0x" + 16 hex digits string is that
// number, anything else is the hash of the bytes as given with A-Z folded to lower case. Failure caught: the
// game's CSymbol64 used as is for the empty string (it returns its seed) or for a hex-looking name.
void TestConfigSymbolMatchesTheServicesToSymbol() {
  const CaseScope scope("config symbol");
  for (const SymbolVector& v : kSymbolVectors) {
    const uint64_t got = nevr_evr_codec::ConfigSymbol(v.text);
    if (got != v.symbol) std::fprintf(stderr, "config symbol vector %zu bytes: got 0x%016llx want 0x%016llx\n", v.text.size(),
                                      static_cast<unsigned long long>(got), static_cast<unsigned long long>(v.symbol));
    QCHECK(got == v.symbol);
  }
  QCHECK(nevr_evr_codec::ConfigSymbol("") == 0);
  QCHECK(nevr_evr_codec::ConfigSymbol("0x82869f0b37eb4378") == 0x82869f0b37eb4378ULL);  // parsed, not hashed
  QCHECK(nevr_evr_codec::ConfigSymbol("Main_Menu") == nevr_evr_codec::ConfigSymbol("main_menu"));  // folded
  // For plain ASCII names the game's CSymbol64Hash agrees with it; the empty string and a hex-looking name are
  // the cases it does not (it returns its seed, and hashes the digits).
  QCHECK(EchoVR::CSymbol64Hash("main_menu") == nevr_evr_codec::ConfigSymbol("main_menu"));
  QCHECK(EchoVR::CSymbol64Hash("Active_Store_Entry") == nevr_evr_codec::ConfigSymbol("Active_Store_Entry"));
  QCHECK(EchoVR::CSymbol64Hash("") != nevr_evr_codec::ConfigSymbol(""));
  QCHECK(EchoVR::CSymbol64Hash("0x82869f0b37eb4378") != nevr_evr_codec::ConfigSymbol("0x82869f0b37eb4378"));
}

// The key of a config request is what the service's decoder reads out of it, hashed the way the service
// hashes it for the reply. Failure caught: a different escape, case, duplicate-key or null rule from the
// service's, which would send a reply to the wrong socket or drop it.
void TestConfigRequestKeyMatchesTheServicesDecoder() {
  const CaseScope scope("config request key");
  for (const KeyVector& v : kRequestVectors) {
    for (const bool padded : {true, false}) {
      const std::string payload = padded ? std::string(1, '\0') + v.json : v.json;
      if (!padded && (v.json.empty() || v.json[0] != '{')) continue;
      const std::optional<nevr_evr_codec::ConfigKey> key = nevr_evr_codec::ParseConfigRequestKey(payload);
      QCHECK(key.has_value() == v.ok);
      if (key.has_value() && v.ok) {
        QCHECK(key->type == v.type);
        QCHECK(key->id == v.id);
      }
    }
  }
  QCHECK(!nevr_evr_codec::ParseConfigRequestKey("").has_value());
  QCHECK(!nevr_evr_codec::ParseConfigRequestKey("x").has_value());  // a pad byte and no JSON
  // The pad byte is whatever the game left there, '{' included: seen live (#708 S2b) on the game's first request,
  // `{` + `{"type":"main_menu","id":"main_menu"}` + NUL, 39 bytes. Failure caught: that request keyed as unreadable, so
  // the service's reply matched nothing and the game never got its main_menu config.
  const std::string braced = std::string(1, '{') + R"({"type":"main_menu","id":"main_menu"})" + std::string(1, '\0');
  const std::optional<nevr_evr_codec::ConfigKey> bracedKey = nevr_evr_codec::ParseConfigRequestKey(braced);
  QCHECK(bracedKey.has_value());
  if (bracedKey.has_value()) {
    QCHECK(bracedKey->type == nevr_evr_codec::ConfigSymbol("main_menu"));
    QCHECK(bracedKey->id == nevr_evr_codec::ConfigSymbol("main_menu"));
  }
}

// The key of a reply: the success carries the two symbols, the failure the two strings.
void TestConfigReplyKeyIsTheRequestsTypeAndId() {
  const CaseScope scope("config reply key");
  using nevr_evr_codec::ConfigKey;
  std::string success;
  for (const uint64_t v : {0x1509ee27440e1d7bULL, 0x794e13955045c77eULL}) {
    for (int i = 0; i < 8; ++i) success.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  success += "zstd-resource-bytes";
  const std::optional<ConfigKey> s = nevr_evr_codec::ParseConfigReplyKey(nevr_evr_codec::kSymConfigSuccess, success);
  QCHECK(s.has_value() && s->type == 0x1509ee27440e1d7bULL && s->id == 0x794e13955045c77eULL);
  for (const KeyVector& v : kFailureVectors) {
    const std::string payload = std::string(16, '\0') + v.json;
    const std::optional<ConfigKey> key = nevr_evr_codec::ParseConfigReplyKey(nevr_evr_codec::kSymConfigFailure, payload);
    QCHECK(key.has_value() == v.ok);
    if (key.has_value() && v.ok) {
      QCHECK(key->type == v.type);
      QCHECK(key->id == v.id);
    }
  }
  QCHECK(!nevr_evr_codec::ParseConfigReplyKey(nevr_evr_codec::kSymConfigSuccess, "short").has_value());
  QCHECK(!nevr_evr_codec::ParseConfigReplyKey(nevr_evr_codec::kSymLoginSuccess, success).has_value());
}

// A hinted role is final, also for the connection a newer login replaces. The PC bridge hints every /login as
// login; when a second one arrives it takes the session's login role and the first is closed, not given a
// matchmaker role it was never hinted. Failure caught: a hinted login demoted to matchmaker and left open.
void TestAHintedLoginReplacedByANewerLoginIsClosed() {
  const CaseScope scope("hinted login replaced");
  Rig rig;
  rig.router->OnGameOpen(1, Hinted(Role::Config));
  rig.router->OnGameOpen(2, Hinted(Role::Login));
  rig.router->OnGameOpen(3, Hinted(Role::Login));  // a newer /login
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1);
  if (closes.size() == 1) {
    QCHECK(closes[0].id == 2);
    QCHECK(closes[0].code == kCloseGoingAway);
  }
  QCHECK(!Exempt(rig, 2));
  QCHECK(Exempt(rig, 3));
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnRemoteFrame(session, Msg(kSymLobbySessionSuccess, "lobby"), true);  // no matchmaker: the new login
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 3);
}

// An unhinted login connection that another connection replaces is given the matchmaker role only while the
// matchmaker cap has room; with the cap full it is closed. Game 1 (config by order) names itself the login while
// game 3 is the one matchmaker the cap allows, so the old login (game 2) has no room.
// Failure caught: the cap bypassed by demoting the old login.
void TestAReplacedLoginIsClosedWhenTheMatchmakerCapIsFull() {
  const CaseScope scope("replaced login at the cap");
  Options o;
  o.limits.maxMatchmakerConnections = 1;
  Rig rig(std::move(o));
  OpenThree(rig);  // config 1, login 2, matchmaker 3: the cap
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(1, FirstFrameOf(Role::Login), true);  // game 1 takes the login over
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1);
  if (closes.size() == 1) QCHECK(closes[0].id == 2);
  QCHECK(!Exempt(rig, 2));
  QCHECK(Exempt(rig, 1));
}

// The login is built when a login connection attaches to the open session, and a build that returns no frame is
// tried again for the NEXT login connection, not for an unrelated connection that opens in between. The first
// login's build returns nothing; a matchmaker then opens (no build); a newer login attaches and its build
// succeeds, sending exactly one login. Failure caught: a build per open (a missing identity retried for every
// connection), or no second chance for the next login.
void TestAFailedLoginBuildIsRetriedOnlyForTheNextLoginConnection() {
  const CaseScope scope("failed login build");
  int builds = 0;
  Options o;
  o.buildLogin = [&builds]() {
    ++builds;
    return builds == 1 ? std::optional<std::string>() : std::optional<std::string>(kLogin);
  };
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1, Hinted(Role::Config));
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameOpen(2, Hinted(Role::Login));  // attaches to the open session: build 1, no frame
  QCHECK(builds == 1);
  QCHECK(rig.logs.Has("login NOT injected", static_cast<int>(LogLevel::Error)));
  rig.router->OnGameOpen(3, Hinted(Role::Matchmaker));  // shares the session: no build
  rig.router->OnGameOpen(4, Hinted(Role::Config));      // a config connection: no build
  QCHECK(builds == 1);
  QCHECK(CountUpstreamAnywhere(rig, nevr_evr_codec::kSymLoginRequest) == 0);
  rig.router->OnGameOpen(5, Hinted(Role::Login));  // the next login connection: build 2
  QCHECK(builds == 2);
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 1);
}

// An unhinted connection that proves to be the login by its first frame becomes the login connection and the
// login build is tried for it; a connection that proves to be anything else does not try it. The provisional
// login's build returns no frame; a matchmaker by order then names itself config (no build); the config socket
// by order names itself login (build 2, which succeeds, and sends one login).
// Failure caught: a build per relabel.
void TestARelabelToLoginTriesTheLoginBuildAndOtherRelabelsDoNot() {
  const CaseScope scope("relabel login build");
  int builds = 0;
  Options o;
  o.buildLogin = [&builds]() {
    ++builds;
    return builds == 1 ? std::optional<std::string>() : std::optional<std::string>(kLogin);
  };
  Rig rig(std::move(o));
  OpenThree(rig);  // config 1, login 2, matchmaker 3
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);  // the provisional login is attached: build 1, no frame
  QCHECK(builds == 1);
  rig.router->OnGameFrame(3, FirstFrameOf(Role::Config), true);  // a relabel to config
  QCHECK(builds == 1);
  rig.router->OnGameFrame(1, FirstFrameOf(Role::Login), true);   // a relabel to login
  QCHECK(builds == 2);
  QCHECK(CountUpstream(rig, session, nevr_evr_codec::kSymLoginRequest) == 2);  // the injected one and game 1's own
}

// One login per session: a login that was sent is not sent again when a newer login connection takes the
// session's login role over (the PC bridge hints every /login as login). Failure caught: a second LoginRequest
// on a session that already logged in.
void TestANewerLoginConnectionDoesNotSendASecondLogin() {
  const CaseScope scope("one login per session");
  int builds = 0;
  Options o;
  o.buildLogin = [&builds]() {
    ++builds;
    return std::optional<std::string>(kLogin);
  };
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1, Hinted(Role::Config));
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameOpen(2, Hinted(Role::Login));  // attaches to the open session: the login is sent
  QCHECK(builds == 1);
  rig.router->OnGameOpen(3, Hinted(Role::Login));  // a newer /login takes the login role over
  QCHECK(builds == 1);
  QCHECK(CountUpstreamAnywhere(rig, nevr_evr_codec::kSymLoginRequest) == 1);
  QCHECK(Exempt(rig, 3));
}

// A hinted role is final. A hinted config socket whose first frame is LogInRequestv2 stays config: the
// contradiction is logged once, at Warning, and nothing is relabelled or taken over. The hinted login
// socket keeps the exemption and the login replies, and the config socket still gets config replies.
// Failure caught: the hint ignored (today's relabel moves the socket to login and demotes the real one).
void TestAHintedConfigSocketThatSendsALoginStaysConfig() {
  const CaseScope scope("hinted role is final");
  Rig rig;
  rig.router->OnGameOpen(1, Hinted(Role::Config));
  rig.router->OnGameOpen(2, Hinted(Role::Login));
  QCHECK(rig.remotes.Opens().size() == 1);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(1, FirstFrameOf(Role::Login), true);  // contradicts the hint
  rig.router->OnGameFrame(1, FirstFrameOf(Role::Login), true);  // the frame is checked once: no second warning
  int warnings = 0;
  {
    std::lock_guard<std::mutex> lock(rig.logs.mutex);
    for (const auto& line : rig.logs.lines) {
      if (line.first == LogLevel::Warning && line.second.find("contradicts the hint") != std::string::npos) ++warnings;
    }
  }
  QCHECK(warnings == 1);
  QCHECK(!rig.logs.Has("-> login"));  // not relabelled
  QCHECK(Exempt(rig, 2));
  QCHECK(!Exempt(rig, 1));
  rig.router->OnGameFrame(1, FirstFrameOf(Role::Config), true);
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);  // -> login
  rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymConfigSuccess, "config"), true);              // -> config
  const std::vector<Delivery> expected = {
      {2, nevr_evr_codec::kSymLoginSuccess},
      {1, nevr_evr_codec::kSymConfigSuccess},
  };
  const std::vector<Delivery> got = Delivered(rig);
  QCHECK(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) QCHECK(got[i] == expected[i]);
  QCHECK(rig.games.Closes().empty());
}

}  // namespace

// ---- login removal notice after a lost session (#320, option B) -----------------------------------------
// On Quest the game sends its own login. When the remote session ends with nothing outstanding on the login
// connection the game reconnects its login socket WITHOUT logging in again. Instead of replaying a credential
// the router sends the game an SNSLoginRemovedNotify for the account it last saw logged in, on that login
// socket, once per lost session; the game puts itself on the login-failed screen and RETRY logs it in.

const std::string kRemovedJson = "{\"message\":\"Connection lost. Select RETRY to sign in again.\"}";
constexpr uint64_t kTestPlatform = 4;
constexpr uint64_t kTestAccount = 0x1122334455667788ULL;

Options RemovalOptions() {
  Options o;
  o.loginRemovedJson = kRemovedJson;  // no buildLogin: the game's own login is the only login
  return o;
}

std::string ExpectedRemoved() {
  return nevr_evr_codec::BuildLoginRemovedNotify({kTestPlatform, kTestAccount}, nevr_evr_codec::kLoginRemovedReasonText,
                                                 kRemovedJson);
}

// Config (1), login (2) and matchmaker (3) connections; the login session open, the game's own login sent and
// answered by a LoginSuccess whose Unrequire is in the same frame (nothing outstanding afterwards).
RemoteId EstablishedForRemoval(Rig& rig) {
  OpenThree(rig);
  const RemoteId session = SessionRemote(rig);
  rig.router->OnRemoteOpen(session);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest, "GAME-LOGIN-" + kSecret), true);
  rig.router->OnRemoteFrame(session,
                            nevr_evr_codec::BuildLoginSuccess(kTestPlatform, kTestAccount) +
                                Msg(nevr_evr_codec::kSymConnectionUnrequire, "u"),
                            true);
  return session;
}

// The session fails under the game; its sockets close and the game's login socket comes back as game 4.
RemoteId LoseAndReconnect(Rig& rig, RemoteId failed) {
  rig.router->OnRemoteError(failed, 0, "network error");
  rig.router->OnGameClose(2);
  rig.router->OnGameClose(3);
  const std::size_t before = rig.remotes.Opens().size();
  rig.router->OnGameOpen(4);
  const auto opens = rig.remotes.Opens();
  return opens.size() == before + 1 ? opens.back().remote : kNoRemote;
}

std::vector<std::string> SentToGame(Rig& rig, GameId game) {
  std::vector<std::string> out;
  for (const SentFrame& f : rig.games.Sent()) {
    if (f.id == game) out.push_back(f.data);
  }
  return out;
}

int CountNotices(Rig& rig) {
  int n = 0;
  for (const SentFrame& f : rig.games.Sent()) {
    if (nevr_evr_codec::FirstSymbol(f.data) == nevr_evr_codec::kSymLoginRemovedNotify) ++n;
  }
  return n;
}

// The fixed part is 0x18 bytes. Run first in main(): with a shorter constant BuildLoginRemovedNotify throws
// (the padding length underflows), which would end the process before any CHECK printed. Failure caught: a
// changed fixed size is reported as a failed check, not as an abort.
void TestLoginRemovedFixedSizeIsPinned() {
  QCHECK(nevr_evr_codec::kLoginRemovedFixedSize == 0x18);
}

// The frame layout the game's handler reads (libr15 ListenProxy 0x1938358: size - 0x18 is the JSON; callbacks
// 0x1933a28 / 0x125f908). Failure caught: a shifted offset, a wrong size, the id words in the wrong order.
void TestLoginRemovedFrameLayout() {
  const std::string frame = ExpectedRemoved();
  nevr_evr_codec::Message m;
  QCHECK(nevr_evr_codec::ReadMessage(frame, 0, &m) == nevr_evr_codec::ReadStatus::Ok);
  QCHECK(m.symbol == nevr_evr_codec::kSymLoginRemovedNotify && m.symbol == 0x73c0a8cbf5c697abULL);
  QCHECK(m.length == nevr_evr_codec::kLoginRemovedFixedSize + kRemovedJson.size());
  QCHECK(nevr_evr_codec::kLoginRemovedFixedSize == 0x18);
  const uint8_t* p = m.payload;
  auto le64 = [](const uint8_t* q) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | q[i];
    return v;
  };
  QCHECK(!nevr_evr_codec::kLoginRemovedUserIdSwapped);
  QCHECK(le64(p) == kTestPlatform && le64(p + 8) == kTestAccount);  // the EvrId as LoginSuccess carries it
  const uint32_t word10 = p[0x10] | (p[0x11] << 8) | (p[0x12] << 16) | (static_cast<uint32_t>(p[0x13]) << 24);
  QCHECK(word10 == nevr_evr_codec::kLoginRemovedWord10 && word10 == 0);  // pinned: changing the unmeasured word is an edit here too
  QCHECK(p[0x14] == nevr_evr_codec::kLoginRemovedReasonText && p[0x14] == 1);
  QCHECK(p[0x15] == 0 && p[0x16] == 0 && p[0x17] == 0);
  const std::string json(reinterpret_cast<const char*>(p + 0x18), static_cast<std::size_t>(m.length - 0x18));
  QCHECK(json == kRemovedJson);
}

// LoginSuccess names the account at payload 16..32 (after the session UUID).
void TestLoginSuccessUserIdIsParsed() {
  const auto id = nevr_evr_codec::ParseLoginSuccessUserId(nevr_evr_codec::BuildLoginSuccess(kTestPlatform, kTestAccount));
  QCHECK(id.has_value() && id->platformCode == kTestPlatform && id->accountId == kTestAccount);
  QCHECK(!nevr_evr_codec::ParseLoginSuccessUserId(Msg(nevr_evr_codec::kSymLoginSuccess, "short")).has_value());
  QCHECK(!nevr_evr_codec::ParseLoginSuccessUserId(Msg(kSymSomething, std::string(40, 'x'))).has_value());
}

// Failure caught: the reconnected login socket left on an unauthenticated session with a game that believes it
// is logged in. The silent login socket gets exactly one notice, with the account the service logged in, and
// the new session is not sent the old login (nothing is replayed, nothing credential-like is kept).
void TestSilentLoginSocketAfterTheLossIsSentOneNotice() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  QCHECK(rig.router->GetStats().loginUserKnown);
  const RemoteId fresh = LoseAndReconnect(rig, session);
  QCHECK(fresh != kNoRemote);
  QCHECK(rig.router->GetStats().loginRemovedDue);
  QCHECK(rig.logs.Has("login-removed notice"));
  QCHECK(SentToGame(rig, 4).empty());  // nothing yet: the connection has not shown it is the login socket
  rig.router->OnRemoteOpen(fresh);
  rig.router->OnGameSilent(4);
  const auto toGame = SentToGame(rig, 4);
  QCHECK(toGame.size() == 1 && toGame[0] == ExpectedRemoved());
  QCHECK(rig.router->GetStats().loginsRemoved == 1);
  QCHECK(rig.router->GetStats().loginRemovedDue);  // stays armed: this socket may not be the login peer
  rig.router->OnGameSilent(4);  // idempotent
  QCHECK(CountNotices(rig) == 1);
  // The new session was never sent the game's old login or anything else of its own.
  for (const SentFrame& f : rig.remotes.Sent()) {
    if (f.id == fresh) QCHECK(f.data != Msg(nevr_evr_codec::kSymLoginRequest, "GAME-LOGIN-" + kSecret));
  }
  QCHECK(!rig.logs.Has(kSecret));
}

// Failure caught (the review's HIGH): the router cannot tell the login socket from the others, so a notice
// consumed by a guess left the real login socket on the unauthenticated session. Every socket that reconnects
// is sent it, once. The game drops it on any peer but its login peer (CNSUser::LoginRemovedCB 0x1933a64).
void TestConfigSocketFirstThenTheQuietLoginSocketEachGetOneNotice() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  const RemoteId provisional = LoseAndReconnect(rig, session);  // game 4: provisional login role
  rig.router->OnRemoteOpen(provisional);
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymConfigRequest, "cfg"), true);  // it is the config socket
  QCHECK(SentToGame(rig, 4).size() == 1 && SentToGame(rig, 4)[0] == ExpectedRemoved());
  QCHECK(rig.router->GetStats().loginRemovedDue);  // still armed: the login socket has not been heard
  rig.router->OnGameOpen(5);                       // the real login socket (a matchmaker by order), quiet
  rig.router->OnGameSilent(5);
  QCHECK(SentToGame(rig, 5).size() == 1 && SentToGame(rig, 5)[0] == ExpectedRemoved());
  // Never twice to one socket, whatever it does next.
  rig.router->OnGameSilent(5);
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymConfigRequest, "cfg2"), true);
  rig.router->OnGameFrame(5, Msg(kSymSomething, "profile"), true);
  QCHECK(SentToGame(rig, 4).size() == 1 && SentToGame(rig, 5).size() == 1);
  QCHECK(CountNotices(rig) == 2 && rig.router->GetStats().loginsRemoved == 2);
}

// The loss closes every socket on the session, so a socket that was open before it can only be a closed one, and
// a closed socket is never sent the notice (it is for sockets that reconnect). Failure caught: a notice queued
// to a socket the router has closed.
void TestASocketTheLossClosedIsNeverSentTheNotice() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  rig.router->OnGameFrame(1, Msg(nevr_evr_codec::kSymConfigRequest, "cfg"), true);  // game 1 predates the loss
  const RemoteId fresh = LoseAndReconnect(rig, session);
  bool gameOneClosed = false;
  for (const CloseRecord& c : rig.games.Closes()) gameOneClosed = gameOneClosed || c.id == 1;
  QCHECK(gameOneClosed);
  rig.router->OnRemoteOpen(fresh);
  rig.router->OnGameFrame(1, Msg(nevr_evr_codec::kSymConfigRequest, "cfg-again"), true);
  rig.router->OnGameSilent(1);
  QCHECK(SentToGame(rig, 1).empty());
  rig.router->OnGameSilent(4);
  QCHECK(SentToGame(rig, 4).size() == 1);
}

// The review's path (a): a matchmaker socket reconnects first and stays quiet; the real login socket (a
// matchmaker by order, silent) comes after. Each gets exactly one.
void TestQuietMatchmakerFirstThenTheLoginSocketEachGetOneNotice() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  const RemoteId fresh = LoseAndReconnect(rig, session);  // game 4: quiet
  rig.router->OnRemoteOpen(fresh);
  rig.router->OnGameOpen(5);  // game 5: the real login socket, a matchmaker by order, quiet
  rig.router->OnGameSilent(4);
  rig.router->OnGameSilent(5);
  QCHECK(SentToGame(rig, 4).size() == 1 && SentToGame(rig, 4)[0] == ExpectedRemoved());
  QCHECK(SentToGame(rig, 5).size() == 1 && SentToGame(rig, 5)[0] == ExpectedRemoved());
  QCHECK(CountNotices(rig) == 2);
}

// The review's path (b): the real login socket is guessed as a matchmaker and the first socket spoke as
// matchmaker; the notice must still reach the real one (a notice on a socket that spoke is not the last).
void TestRealLoginSocketGuessedAsMatchmakerStillGetsItsNotice() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  const RemoteId fresh = LoseAndReconnect(rig, session);  // game 4: provisional login
  rig.router->OnRemoteOpen(fresh);
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymFindSessionRequest, "find"), true);  // a matchmaker
  QCHECK(SentToGame(rig, 4).size() == 1);
  rig.router->OnGameOpen(5);  // the real login socket, a matchmaker by order
  QCHECK(SentToGame(rig, 5).empty());
  rig.router->OnGameSilent(5);
  QCHECK(SentToGame(rig, 5).size() == 1 && SentToGame(rig, 5)[0] == ExpectedRemoved());
  QCHECK(CountNotices(rig) == 2);
}

// A silent matchmaker by order is sent it too: it cannot be told from the real login socket.
void TestSilentMatchmakerByOrderIsSentOneNoticeLikeTheRest() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  const RemoteId fresh = LoseAndReconnect(rig, session);
  rig.router->OnRemoteOpen(fresh);
  rig.router->OnGameOpen(5);  // a second connection on the live session: matchmaker role
  rig.router->OnGameSilent(5);
  QCHECK(SentToGame(rig, 5).size() == 1);
  rig.router->OnGameSilent(4);
  QCHECK(SentToGame(rig, 4).size() == 1);
  QCHECK(CountNotices(rig) == 2);
}

// Failure caught (the RETRY path): the login connection ended with a request outstanding. The game takes its
// own Lost path (-95, RETRY runs its login); a notice would only get in front of it.
void TestNoNoticeWhenTheLoginConnectionHadRequestsOutstanding() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  rig.router->OnGameFrame(2, Msg(kSymSomething, "profile-request"), true);  // raises the count, no Unrequire
  const RemoteId fresh = LoseAndReconnect(rig, session);
  QCHECK(fresh != kNoRemote);
  QCHECK(!rig.router->GetStats().loginRemovedDue);
  rig.router->OnRemoteOpen(fresh);
  rig.router->OnGameSilent(4);
  QCHECK(SentToGame(rig, 4).empty());
  QCHECK(rig.router->GetStats().loginsRemoved == 0);
}

// Failure caught: a game that logs in itself being sent a notice that pulls it off the screen it is using.
void TestOwnLoginFirstGetsNoNotice() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  LoseAndReconnect(rig, session);
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymLoginRequest, "OWN-" + kSecret), true);
  QCHECK(SentToGame(rig, 4).empty());
  QCHECK(!rig.router->GetStats().loginRemovedDue);
  QCHECK(rig.logs.Has("login removed notice disarmed"));
  rig.router->OnGameSilent(4);  // ignored: it has spoken
  QCHECK(SentToGame(rig, 4).empty());
  // Disarmed for every socket: one that reconnects (or speaks) after the game's own login is sent nothing.
  rig.router->OnGameOpen(5);
  rig.router->OnGameSilent(5);
  rig.router->OnGameFrame(5, Msg(kSymSomething, "profile"), true);
  QCHECK(SentToGame(rig, 5).empty() && CountNotices(rig) == 0);
}

// The next session being established (LoginSuccess on it) disarms the notice for sockets that come later.
void TestLoginSuccessOnTheNextSessionDisarmsTheNotice() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  const RemoteId fresh = LoseAndReconnect(rig, session);
  rig.router->OnRemoteOpen(fresh);
  QCHECK(rig.router->GetStats().loginRemovedDue);
  rig.router->OnRemoteFrame(fresh, nevr_evr_codec::BuildLoginSuccess(kTestPlatform, kTestAccount), true);
  QCHECK(!rig.router->GetStats().loginRemovedDue);
  rig.router->OnGameOpen(5);
  rig.router->OnGameSilent(5);
  rig.router->OnGameSilent(4);
  QCHECK(CountNotices(rig) == 0);
}

// The review's LOW: the login socket closed itself before the failure, so nothing reconnects onto it and
// `outstanding` reads false; the notice is not armed.
void TestNoNoticeWhenTheLoginSocketAlreadyClosed() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  rig.router->OnGameClose(2);  // the login socket closes itself; the session lives on for the matchmaker
  rig.router->OnRemoteError(session, 0, "network error");
  QCHECK(!rig.router->GetStats().loginRemovedDue);
  rig.router->OnGameClose(3);
  rig.router->OnGameOpen(4);
  rig.router->OnGameSilent(4);
  QCHECK(CountNotices(rig) == 0);
}

// Failure caught: a login-role request (no login) after the reconnect: the notice goes to that connection.
void TestLoginRoleRequestAfterTheReconnectGetsTheNotice() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  LoseAndReconnect(rig, session);
  rig.router->OnGameFrame(4, Msg(kSymSomething, "profile-request"), true);
  const auto toGame = SentToGame(rig, 4);
  QCHECK(toGame.size() == 1 && toGame[0] == ExpectedRemoved());
}

// Failure caught: the account outliving the player's logout, a rejected login, or the router.
void TestTheAccountIdIsClearedByLogOutRejectionAndShutdown() {
  {  // dropped: the session already ended and the game's login socket has no live remote
    Rig rig(RemovalOptions());
    const RemoteId session = EstablishedForRemoval(rig);
    rig.router->OnRemoteError(session, 0, "network error");
    QCHECK(rig.router->GetStats().loginUserKnown && rig.router->GetStats().loginRemovedDue);
    rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLogOut, "bye"), true);  // dropped: no live session
    QCHECK(!rig.router->GetStats().loginUserKnown);
    QCHECK(!rig.router->GetStats().loginRemovedDue);
    QCHECK(rig.router->GetStats().droppedGameFrames == 1);
  }
  {  // on a connection that is not the login connection
    Rig rig(RemovalOptions());
    EstablishedForRemoval(rig);
    rig.router->OnGameFrame(3, Msg(nevr_evr_codec::kSymLogOut, "bye"), true);
    QCHECK(!rig.router->GetStats().loginUserKnown);
  }
  {  // embedded after another message
    Rig rig(RemovalOptions());
    EstablishedForRemoval(rig);
    rig.router->OnGameFrame(2, Msg(kSymSomething, "x") + Msg(nevr_evr_codec::kSymLogOut, "bye"), true);
    QCHECK(!rig.router->GetStats().loginUserKnown);
  }
  {  // after a logout nothing is sent at the reconnect
    Rig rig(RemovalOptions());
    const RemoteId session = EstablishedForRemoval(rig);
    rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLogOut, "bye"), true);
    const RemoteId fresh = LoseAndReconnect(rig, session);
    rig.router->OnRemoteOpen(fresh);
    rig.router->OnGameSilent(4);
    QCHECK(SentToGame(rig, 4).empty());
  }
  {  // a rejected login
    Rig rig(RemovalOptions());
    OpenThree(rig);
    const RemoteId session = SessionRemote(rig);
    rig.router->OnRemoteOpen(session);
    rig.router->OnRemoteFrame(session, nevr_evr_codec::BuildLoginSuccess(kTestPlatform, kTestAccount), true);
    QCHECK(rig.router->GetStats().loginUserKnown);
    rig.router->OnRemoteFrame(session, Msg(nevr_evr_codec::kSymLoginFailure, std::string(24, '\0')), true);
    QCHECK(!rig.router->GetStats().loginUserKnown);
  }
  {  // Shutdown
    Rig rig(RemovalOptions());
    EstablishedForRemoval(rig);
    QCHECK(rig.router->GetStats().loginUserKnown);
    rig.router->Shutdown();
    QCHECK(!rig.router->GetStats().loginUserKnown);
    QCHECK(!rig.router->GetStats().loginRemovedDue);
  }
}

// Failure caught: the feature on for a wiring that did not ask for it (the PC bridge).
void TestNoNoticeWhenTheOptionIsOff() {
  Rig rig;  // default Options
  const RemoteId session = EstablishedForRemoval(rig);
  QCHECK(!rig.router->GetStats().loginUserKnown);
  const RemoteId fresh = LoseAndReconnect(rig, session);
  rig.router->OnRemoteOpen(fresh);
  rig.router->OnGameSilent(4);
  QCHECK(CountNotices(rig) == 0);
  QCHECK(rig.router->GetStats().loginsRemoved == 0);
}

// One notice per lost session, and again after the next loss.
void TestOneNoticePerLostSession() {
  Rig rig(RemovalOptions());
  const RemoteId session = EstablishedForRemoval(rig);
  const RemoteId second = LoseAndReconnect(rig, session);
  rig.router->OnRemoteOpen(second);
  rig.router->OnGameSilent(4);
  QCHECK(CountNotices(rig) == 1);
  // The game now logs in itself (RETRY) on the same session, and is logged in again.
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymLoginRequest, "RETRY-" + kSecret), true);
  rig.router->OnRemoteFrame(second,
                            nevr_evr_codec::BuildLoginSuccess(kTestPlatform, kTestAccount) +
                                Msg(nevr_evr_codec::kSymConnectionUnrequire, "u"),
                            true);
  QCHECK(CountNotices(rig) == 1);
  rig.router->OnRemoteError(second, 0, "network error again");
  rig.router->OnGameClose(4);
  rig.router->OnGameOpen(6);
  rig.router->OnGameSilent(6);
  QCHECK(CountNotices(rig) == 2);
  QCHECK(SentToGame(rig, 6).size() == 1);
}

int main() {
  TestLoginRemovedFixedSizeIsPinned();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "session_router_test: the login-removed frame size is not 0x18; stopping before it is built\n");
    return 1;
  }
  TestConnectionIdentity();
  TestLobbyFramesFollowTheMatchmakerThatAsked();
  TestGateDefersLoginInjectionOnAnAlreadyOpenSession();
  TestGateRefusedEndsADeferredInjection();
  TestSessionEndedCallbackFiresOncePerSession();
  TestLoginFailureReplacementIsDeliveredAndNotSubscribed();
  TestSendToSessionQueuesBehindTheLoginAndThenSends();
  TestGateAwaitingWhenTheSessionOpensDefersTheLogin();
  TestLobbyReplyGoesToTheLastAskerWhenTwoAreMidRequest();
  TestStandaloneMatchmakerRemoteEndDoesNotEndTheSession();
  TestLoginIsFirstThenQueuedFramesInOrder();
  TestFrameDuringLoginBuildStaysBehindLogin();
  TestLoginInjectedOncePerLoginSessionOnly();
  TestNoLoginFrameWhenIdentityMissing();
  TestThrowingBuilderIsContained();
  TestRemoteCloseClosesGameSocketsWithoutTheLock();
  TestCloseMayReenterRouter();
  TestRemoteErrorBeforeOpenClosesGame();
  TestRemoteOpenRefusedClosesGame();
  TestGameCloseEndsItsStandaloneRemoteButNotTheSession();
  TestRemoteFramesFollowTheActiveGameAndFallBack();
  TestLoginSuccessAndFailureHandling();
  TestNoSecretsInLogs();
  TestOversizedGameFrame();
  TestOversizedRemoteFrame();
  TestPendingQueueIsBounded();
  TestPendingBytesAreBounded();
  TestRemoteBackpressureKeepsOrderAndResumes();
  TestRemoteBackpressureOverflowEndsTheSession();
  TestRemoteSendFailureEndsTheSession();
  TestGameBackpressureKeepsOrder();
  TestGameBackpressureOverflowClosesTheGame();
  TestConcurrentProducersKeepPerSourceOrder();
  TestQuestDefaultsInjectNothing();
  TestLoginRemovedFrameLayout();
  TestLoginSuccessUserIdIsParsed();
  TestSilentLoginSocketAfterTheLossIsSentOneNotice();
  TestConfigSocketFirstThenTheQuietLoginSocketEachGetOneNotice();
  TestASocketTheLossClosedIsNeverSentTheNotice();
  TestQuietMatchmakerFirstThenTheLoginSocketEachGetOneNotice();
  TestRealLoginSocketGuessedAsMatchmakerStillGetsItsNotice();
  TestSilentMatchmakerByOrderIsSentOneNoticeLikeTheRest();
  TestNoNoticeWhenTheLoginConnectionHadRequestsOutstanding();
  TestOwnLoginFirstGetsNoNotice();
  TestLoginSuccessOnTheNextSessionDisarmsTheNotice();
  TestNoNoticeWhenTheLoginSocketAlreadyClosed();
  TestLoginRoleRequestAfterTheReconnectGetsTheNotice();
  TestTheAccountIdIsClearedByLogOutRejectionAndShutdown();
  TestNoNoticeWhenTheOptionIsOff();
  TestOneNoticePerLostSession();
  TestMatchmakerConnectionsAreCapped();
  TestExtraConnectionNeverBecomesASecondLogin();
  TestShutdown();
  TestClassifyFirstFrameTable();
  TestSmokeSequenceNewConfigSocketIsConfigAndProfileReplyReachesLogin();
  TestServerFramesRouteByRole();
  TestInterleavedUnrequiresReachTheConnectionThatOwesThem();
  TestPingDiscoveryAfterLoginWithoutAMatchmakerDoesNotWrapTheLoginCount();
  TestChannelInfoResponseGoesToTheLoginConnectionWhileAMatchmakerExists();
  TestUnflaggedRequestsDoNotCountAndAnExtraUnrequireIsDropped();
  TestConfigConnectionUnrequireNeverExceedsItsRequests();
  TestTheUnrequireInsideTheLoginReplyFrameLowersTheLoginCount();
  TestALoginFailureAndItsUnrequireLowerTheLoginCount();
  TestAnEmbeddedUnrequireTheCountCannotCoverIsCounted();
  TestUnknownFirstFrameKeepsTheProvisionalRole();
  TestReconnectedLoginConnectionTakesOverTheSession();
  TestLoginReplyWithoutALoginConnectionIsDropped();
  TestProvisionalLoginThatSendsConfigIsRelabelledInPlace();
  TestLoginConnectionWithSharersIsRelabelledInPlace();
  TestLoginRequestOnAnotherConnectionTakesOverFromALiveLogin();
  TestHeldSessionOpensWhenTheAccountAppears();
  TestConfigConnectionFailsAtOnceWhileTheAccountIsAwaited();
  TestConfigAndLoginShareOneRemoteOnceTheAccountExists();
  TestHeldSessionIsClosedWhenTheAccountIsRefused();
  TestReplacementLoginConnectionIsHeldOnTheSameSession();
  TestNoGateNeverHolds();
  TestOnlyTheLoginConnectionIsIdleExempt();
  // #578: the one-session and role-hint rules.
  TestUnhintedAllOpenOrdersShareOneSession();
  TestUnhintedFirstFrameOrderVariedSeparatelyFromOpenOrder();
  TestIdleExemptionFollowsTheRoleThroughARelabel();
  TestHintedAllOpenOrdersOneSessionOneLogin();
  TestHintedSessionRemoteOpeningBeforeTheLoginSocketAttaches();
  TestConfigClosesAndReopensAfterLoginOnTheSameSession();
  TestSessionEndClosesConfigAndConfigReconnectsToANewSession();
  TestTwoConfigSocketsAskingTheSameResourceAreAnsweredOldestFirst();
  TestAnUnansweredConfigRequestDoesNotShiftLaterReplies();
  TestAConfigFailureIsMatchedByItsStrings();
  TestAConfigReplyNobodyAskedForIsDroppedAndLogged();
  TestAnOrphanedConfigReplyIsLoggedEveryTime();
  TestTheListOfOpenConfigRequestsIsBoundedAndSaysWhenItForgets();
  TestAClosedConfigSocketsStaleEntryYieldsToItsReplacement();
  TestAStaleConfigEntryDoesNotStarveReplacementSockets();
  TestTwoRequestsOnAReplacementSocketBehindAStaleEntryAreBothAnswered();
  TestAClosedSocketsReplyIsDroppedWhenNoLiveSocketAskedForIt();
  TestStaleEntriesAreSkippedToReachALiveRequest();
  TestAHandOverBehindAnOlderOpenRequestErasesTheStaleEntryAndTheLiveOne();
  TestAHandOverWithEntriesAfterTheLiveOneAnswersEveryResource();
  TestASessionEndForgetsTheOpenConfigRequests();
  TestAConfigDropWritesOnlyItsOwnLine();
  TestRequestsOfOneTypeWithDifferentIdsAreToldApart();
  TestConfigFailureGoesToTheConfigSocket();
  TestHeldSessionQueuesLoginThenFlushesOnReady();
  TestHeldSessionPurgesAClosedLoginSocketsQueuedFrames();
  TestTheNinthHintedMatchmakerIsRefused();
  TestConfigSymbolMatchesTheServicesToSymbol();
  TestConfigRequestKeyMatchesTheServicesDecoder();
  TestConfigReplyKeyIsTheRequestsTypeAndId();
  TestAHintedConfigSocketThatSendsALoginStaysConfig();
  TestAHintedLoginReplacedByANewerLoginIsClosed();
  TestAReplacedLoginIsClosedWhenTheMatchmakerCapIsFull();
  TestAFailedLoginBuildIsRetriedOnlyForTheNextLoginConnection();
  TestARelabelToLoginTriesTheLoginBuildAndOtherRelabelsDoNot();
  TestANewerLoginConnectionDoesNotSendASecondLogin();
  TestTheDemotedLoginConnectionRidesTheSessionAsAMatchmaker();
  TestFramesWaitBehindTheLoginBeingBuiltExceptConfigs();
  TestAClosedSocketsQueuedBytesDoNotCountAgainstTheLimit();
  TestAHintedConfigSocketIsNotRefusedByTheMatchmakerCap();
  TestRelabelToMatchmakerChecksTheCap();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "session_router_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("session_router_test: all checks passed\n");
  return 0;
}
