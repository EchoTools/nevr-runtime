// Host test for the shared EVR session router (src/runtime/compat/session_router.{h,cpp}), driven
// through fake game and remote transports. No sockets, no TLS, no game. The same source compiles with
// the NDK (src/quest/CMakeLists.txt) and runs on the host via `just test-quest-router`.
//
// Each test states the failure it exists to catch. The mutation proof for every one of them is recorded
// in the PR body.

#include <algorithm>
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

#include "quest/tests/test_check.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/session_router.h"

using namespace nevr_session_router;

namespace {

constexpr uint64_t kSymSomething = 0x1111222233334444ULL;  // an arbitrary game message
constexpr uint64_t kSymLobbySessionSuccess = 0x6d4de3650ee3110fULL;  // SNSLobbySessionSuccessv5
constexpr uint64_t kSymConfigSuccess = 0xb9cdaf586f7bd012ULL;         // SNSConfigSuccessv2
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

// ---------------------------------------------------------------------------------------------
// Identity: the Nth connection is config, login, matchmaker; matchmakers share the login remote.
void TestConnectionIdentity() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  const auto opens = rig.remotes.Opens();
  QCHECK(opens.size() == 2);  // config and login each get a remote; the matchmaker does not
  if (opens.size() == 2) {
    QCHECK(opens[0].connIdx == 0 && opens[0].role == Role::Config);
    QCHECK(opens[1].connIdx == 1 && opens[1].role == Role::Login);
    QCHECK(opens[0].remote != opens[1].remote);
    QCHECK(!opens[1].standaloneMatchmaker);
  }
  QCHECK(rig.router->GetStats().games == 3);
  QCHECK(rig.router->GetStats().remotes == 2);
}

// Failure caught: the config connection's remote dying being treated as the end of the login session
// (which would reset connection numbering and close the login and matchmaker sockets).
void TestConfigRemoteEndDoesNotEndTheLoginSession() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  const auto opens = rig.remotes.Opens();
  rig.router->OnRemoteClose(opens[0].remote, 1000);
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1 && closes[0].id == 1);  // only the config socket
  QCHECK(rig.router->GetStats().nextConnIdx == 3);   // numbering untouched
  QCHECK(rig.router->GetStats().remotes == 1);
}

// ---------------------------------------------------------------------------------------------
// Login injection ordering. Failure caught: a game frame reaching the server before the LoginRequest.
void TestLoginIsFirstThenQueuedFramesInOrder() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId loginRemote = rig.remotes.Opens()[1].remote;
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
  rig.router->OnRemoteOpen(rig.remotes.Opens()[1].remote);
  const auto sent = rig.remotes.Sent();
  QCHECK(sent.size() == 2);
  if (sent.size() == 2) {
    QCHECK(sent[0].data == kLogin);
    QCHECK(sent[1].data == late);
  }
}

// Failure caught: a second login on the same session (OnRemoteOpen delivered twice), or a login on the
// config or matchmaker connection.
void TestLoginInjectedOncePerLoginSessionOnly() {
  int builds = 0;
  Options o;
  o.buildLogin = [&]() {
    ++builds;
    return std::optional<std::string>(kLogin);
  };
  Rig rig(std::move(o));
  OpenThree(rig);
  const auto opens = rig.remotes.Opens();
  rig.router->OnRemoteOpen(opens[0].remote);  // config
  rig.router->OnRemoteOpen(opens[1].remote);  // login
  rig.router->OnRemoteOpen(opens[1].remote);  // duplicate event
  QCHECK(builds == 1);
  int logins = 0;
  for (const auto& s : rig.remotes.Sent()) logins += (s.data == kLogin);
  QCHECK(logins == 1);
  for (const auto& s : rig.remotes.Sent()) QCHECK(s.id == opens[1].remote);  // nothing on the config remote
  // The matchmaker rides the login remote and its frames go out after the login, without another login.
  rig.router->OnGameFrame(3, Msg(kSymSomething, "mm"), true);
  QCHECK(builds == 1);
  QCHECK(rig.remotes.Sent().back().id == opens[1].remote);
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
  rig.router->OnRemoteOpen(rig.remotes.Opens()[1].remote);
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
  rig.router->OnRemoteOpen(rig.remotes.Opens()[1].remote);
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
  const auto opens = rig.remotes.Opens();
  rig.router->OnRemoteOpen(opens[1].remote);
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
  rig.router->OnRemoteClose(opens[1].remote, 1006);
  for (auto& t : probeThreads) t.join();
  auto closes = rig.games.Closes();
  std::vector<uint64_t> ids;
  for (const auto& c : closes) ids.push_back(c.id);
  QCHECK(closes.size() == 2);  // login (2) and matchmaker (3); the config socket (1) has its own remote
  QCHECK(std::find(ids.begin(), ids.end(), 2) != ids.end());
  QCHECK(std::find(ids.begin(), ids.end(), 3) != ids.end());
  QCHECK(std::find(ids.begin(), ids.end(), 1) == ids.end());
  for (const auto& c : closes) QCHECK(c.code == kCloseGoingAway);
  QCHECK(probes == 2);
  QCHECK(answered == probes);  // the lock was free during every Close
  QCHECK(rig.router->GetStats().nextConnIdx == 1);
  // The game's next connection is a fresh login on a fresh remote.
  rig.router->OnGameClose(2);
  rig.router->OnGameClose(3);
  rig.router->OnGameOpen(4);
  const auto after = rig.remotes.Opens();
  QCHECK(after.size() == 3);
  if (after.size() == 3) {
    QCHECK(after[2].role == Role::Login);
    QCHECK(after[2].remote != opens[1].remote);
  }
}

// Failure caught: Close() re-entering the router (a transport reporting the close synchronously) deadlocking.
void TestCloseMayReenterRouter() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = rig.remotes.Opens()[1].remote;
  rig.router->OnRemoteOpen(login);
  rig.games.onClose = [&](GameId game) { rig.router->OnGameClose(game); };
  rig.router->OnRemoteClose(login, 1000);
  QCHECK(rig.router->GetStats().games == 1);  // only the config game remains
}

// Failure caught: a remote error before it ever opened (connect refused, TLS verification failed) leaving
// the game waiting on a session that will never open.
void TestRemoteErrorBeforeOpenClosesGame() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  rig.router->OnGameFrame(2, Msg(kSymSomething), true);
  rig.router->OnRemoteError(rig.remotes.Opens()[1].remote, 0, "tls verification failed");
  QCHECK(rig.games.Closes().size() == 1);
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

// Failure caught: closing a remote session also killing the config connection's own remote, and the
// unrelated: the OTHER direction (game closes) not closing a remote only it used.
void TestGameCloseEndsItsPrivateRemoteButNotTheSharedLogin() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  const auto opens = rig.remotes.Opens();
  rig.router->OnGameClose(1);  // config
  auto closes = rig.remotes.Closes();
  QCHECK(closes.size() == 1 && closes[0].id == opens[0].remote);
  rig.router->OnGameClose(3);  // matchmaker on the shared login remote
  QCHECK(rig.remotes.Closes().size() == 1);
  rig.router->OnGameClose(2);  // login game: the session stays for later matchmakers
  QCHECK(rig.remotes.Closes().size() == 1);
  QCHECK(rig.router->GetStats().remotes == 1);
}

// ---------------------------------------------------------------------------------------------
// Routing of remote frames on the shared login session.
void TestRemoteFramesFollowTheActiveGameAndFallBack() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
  rig.router->OnRemoteOpen(login);
  rig.router->OnRemoteFrame(login, std::string(101, 'a'), true);
  QCHECK(rig.games.Sent().empty());
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1 && closes[0].code == kCloseMessageTooBig);
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
  QCHECK(rig.router->GetStats().pendingFrames == 3);
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
  QCHECK(rig.router->GetStats().pendingFrames == 1);
}

// ---------------------------------------------------------------------------------------------
// Backpressure. Failure caught: reordering or losing frames when the transport reports it is full.
void TestRemoteBackpressureKeepsOrderAndResumes() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
  rig.router->OnRemoteOpen(login);
  rig.remotes.script = {SendResult::WouldBlock};
  for (int i = 0; i < 20 && rig.games.Closes().empty(); ++i) rig.router->OnGameFrame(2, std::string(60, 'z'), true);
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1 && closes[0].code == kCloseTryAgainLater);
  QCHECK(rig.router->GetStats().outboundBytes == 0);
  QCHECK(rig.logs.Has("outbound buffer to the remote is full"));
}

// A send the transport reports as failed ends the session; it is not retried and not ignored.
void TestRemoteSendFailureEndsTheSession() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = rig.remotes.Opens()[1].remote;
  rig.router->OnRemoteOpen(login);
  rig.remotes.script = {SendResult::Failed};
  rig.router->OnGameFrame(2, Msg(kSymSomething), true);
  QCHECK(rig.games.Closes().size() == 1);
  QCHECK(rig.router->GetStats().remotes == 1);  // the config remote only
}

void TestGameBackpressureKeepsOrder() {
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  QCHECK(opens.size() == 2);
  rig.router->OnRemoteOpen(opens[0].remote);
  rig.router->OnRemoteOpen(opens[1].remote);
  QCHECK(rig.remotes.Sent().empty());  // opening a session sends nothing by itself
  const std::string gameLogin = Msg(nevr_evr_codec::kSymLoginRequest, "the-game's-own-login");
  rig.router->OnGameFrame(2, gameLogin, true);
  rig.router->OnRemoteFrame(opens[1].remote, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);
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
  rig.router->OnRemoteOpen(before[1].remote);
  for (GameId g = 10; g < 14; ++g) rig.router->OnGameOpen(g);
  QCHECK(rig.remotes.Opens().size() == before.size());  // none of them opened a remote
  int logins = 0;
  for (const auto& s : rig.remotes.Sent()) logins += (s.data == kLogin);
  QCHECK(logins == 1);
  QCHECK(rig.router->GetStats().remotes == 2);
}

// Shutdown closes everything once and later events are harmless.
void TestShutdown() {
  Rig rig(WithLogin(kLogin));
  OpenThree(rig);
  rig.router->Shutdown();
  QCHECK(rig.games.Closes().size() == 3);
  QCHECK(rig.remotes.Closes().size() == 2);
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
  const RemoteId bootConfig = rig.remotes.Opens()[0].remote;
  rig.router->OnGameFrame(1, Msg(nevr_evr_codec::kSymConfigRequest), true);
  rig.router->OnRemoteError(bootConfig, 0, "no account token");  // fail-fast: the router closes game 1
  rig.router->OnGameClose(1);
  rig.router->OnGameOpen(2);  // the login connection (silent until the game sends its LogInRequest)
  const RemoteId login = rig.remotes.Opens()[1].remote;
  QCHECK(rig.remotes.Opens()[1].role == Role::Login);
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest, "login"), true);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);

  rig.router->OnGameOpen(3);  // the post-login config connection: provisionally a matchmaker (order)
  QCHECK(rig.remotes.Opens().size() == 2);  // sharing the login session: no remote yet
  rig.router->OnGameFrame(3, Msg(nevr_evr_codec::kSymConfigRequest), true);
  const auto opens = rig.remotes.Opens();
  QCHECK(opens.size() == 3);
  RemoteId config = kNoRemote;
  if (opens.size() == 3) {
    QCHECK(opens[2].role == Role::Config);
    QCHECK(opens[2].remote != login);
    config = opens[2].remote;
  }
  rig.router->OnRemoteOpen(config);

  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile"), true);
  rig.router->OnRemoteFrame(config, Msg(kSymConfigSuccess, "config"), true);
  const auto sent = rig.games.Sent();
  QCHECK(sent.size() == 3);  // LoginSuccess, profile, config
  if (sent.size() == 3) {
    QCHECK(sent[0].id == 2);
    QCHECK(sent[1].id == 2);  // the profile reply is the login connection's
    QCHECK(sent[2].id == 3);  // the config reply is the config connection's
  }
  // The config request went to the config remote, not the login session.
  bool configOnConfigRemote = false;
  for (const auto& f : rig.remotes.Sent()) {
    if (nevr_evr_codec::FirstSymbol(f.data) == nevr_evr_codec::kSymConfigRequest && f.id == config) configOnConfigRemote = true;
    if (nevr_evr_codec::FirstSymbol(f.data) == nevr_evr_codec::kSymConfigRequest) QCHECK(f.id != login);
  }
  QCHECK(configOnConfigRemote);
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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

// The config connection has a remote of its own; its Unrequire follows the same rule with its own count.
void TestConfigConnectionUnrequireNeverExceedsItsRequests() {
  Rig rig;
  OpenThree(rig);
  const RemoteId config = rig.remotes.Opens()[0].remote;
  rig.router->OnRemoteOpen(config);
  rig.router->OnGameFrame(1, Msg(nevr_evr_codec::kSymConfigRequest), true);
  rig.router->OnRemoteFrame(config, Msg(0xb9cdaf586f7bd012ULL, "config"), true);
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameClose(2);  // the login connection goes
  rig.router->OnGameOpen(4);   // the game's new login connection: the session has no login connection, so it is one
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymLoginRequest), true);
  QCHECK(rig.remotes.Opens().size() == 2);  // it rides the live session: no second login remote
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
  const RemoteId login = rig.remotes.Opens()[1].remote;
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameClose(2);
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile"), true);
  QCHECK(rig.games.Sent().empty());
  QCHECK(rig.router->GetStats().droppedRemoteFrames == 1);
  rig.router->OnRemoteFrame(login, Msg(kSymLobbySessionSuccess, "lobby"), true);
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 3);
}

// Failure caught: a connection that was given the login role by order (the session was just forgotten, so
// the next connection is a login) but opens with a config request staying on the login session. It is
// moved to a config remote of its own and the session it opened is ended.
void TestProvisionalLoginThatSendsConfigIsMoved() {
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);  // provisional login, alone on its session
  const RemoteId provisional = rig.remotes.Opens()[1].remote;
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymConfigRequest), true);
  const auto opens = rig.remotes.Opens();
  QCHECK(opens.size() == 3);
  if (opens.size() == 3) {
    QCHECK(opens[2].role == Role::Config && opens[2].remote != provisional);
  }
  bool closedProvisional = false;
  for (const auto& c : rig.remotes.Closes()) closedProvisional = closedProvisional || c.id == provisional;
  QCHECK(closedProvisional);
  QCHECK(rig.router->GetStats().nextConnIdx == 1);  // the next connection is a login again
  rig.router->OnGameOpen(3);
  QCHECK(rig.remotes.Opens().size() == 4 && rig.remotes.Opens()[3].role == Role::Login);
}

// Failure caught: the login connection of a session other connections ride being torn away from them by
// its own first frame. Its role stays; the refusal is logged.
void TestLoginConnectionWithSharersKeepsItsRole() {
  Rig rig;
  OpenThree(rig);
  const RemoteId login = rig.remotes.Opens()[1].remote;
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymConfigRequest), true);
  QCHECK(rig.remotes.Opens().size() == 2);
  QCHECK(rig.logs.Has("role stays login", static_cast<int>(LogLevel::Warning)));
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoggedInUserProfileSuccess, "profile"), true);
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 2);
}

// Failure caught: a second connection that names itself the login (LogInRequestv2) while the first login
// connection is still open leaving the old one in charge of the login replies.
void TestLoginRequestOnAnotherConnectionTakesOverFromALiveLogin() {
  Rig rig;
  OpenThree(rig);  // config 1, login 2, matchmaker 3
  const RemoteId login = rig.remotes.Opens()[1].remote;
  rig.router->OnRemoteOpen(login);
  rig.router->OnGameOpen(4);  // a live login exists: provisionally a matchmaker
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymLoginRequest), true);
  QCHECK(rig.remotes.Opens().size() == 2);
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

// Failure caught: the login connection opening a remote (and so failing 1011) while no account exists.
// The held connection gets no remote, no close, the transport is told to keep it open, and what the game
// sends meanwhile waits in order; when the account is there the remote opens and the frames follow.
void TestHeldLoginOpensWhenTheAccountAppears() {
  Gate gate;
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);  // config: not held, opens
  rig.router->OnGameOpen(2);  // login: held
  QCHECK(rig.remotes.Opens().size() == 1 && rig.remotes.Opens()[0].role == Role::Config);
  QCHECK(rig.games.Closes().empty());
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
    QCHECK(opens[1].role == Role::Login && !opens[1].standaloneMatchmaker);
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
  QCHECK(rig.games.Closes().empty());
  // Replies reach the login connection once the session is up.
  rig.router->OnRemoteFrame(login, Msg(nevr_evr_codec::kSymLoginSuccess, std::string(32, '\0')), true);
  QCHECK(rig.games.Sent().size() == 1 && rig.games.Sent()[0].id == 2);
}

// Failure caught: the hold being applied to every connection. A config connection with no account still
// fails at once (its remote cannot start): 1011, and the held login is untouched.
void TestConfigConnectionStillFailsFastWhileTheLoginIsHeld() {
  Gate gate;
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.remotes.openResult = false;  // what the bridge answers when there is no account token
  rig.router->OnGameOpen(1);
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1 && closes[0].id == 1 && closes[0].code == kCloseInternalError);
  rig.router->OnGameOpen(2);
  QCHECK(rig.remotes.Opens().size() == 1);  // the login did not call Open
  QCHECK(rig.games.Closes().size() == 1);
  QCHECK(rig.router->GetStats().heldRemotes == 1);
  rig.router->OnGameOpen(3);  // a matchmaker riding the held session is not held itself
  for (const auto& h : rig.games.Holds()) QCHECK(h.first == 2);
}

// Failure caught: a held login that is never released when the sign-in expires or fails: the game would wait
// forever on a connection that can never log in. The hold ends with a close and the session is forgotten.
void TestHeldLoginIsClosedWhenTheAccountIsRefused() {
  Gate gate;
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  gate.Set(LoginGate::Refused);
  rig.router->ReevaluateHeldLogins();
  const auto closes = rig.games.Closes();
  QCHECK(closes.size() == 1 && closes[0].id == 2 && closes[0].code == kCloseInternalError);
  QCHECK(rig.router->GetStats().heldRemotes == 0);
  QCHECK(rig.remotes.Opens().size() == 1);  // never opened
  QCHECK(rig.router->GetStats().nextConnIdx == 1);  // the next connection is a login again
}

// Failure caught: the game's login connection reconnecting while the account is still awaited. The
// replacement is the login connection of the same held session: held again, one remote, opened once.
void TestReplacementLoginConnectionIsHeldOnTheSameSession() {
  Gate gate;
  Options o;
  o.loginGate = gate.Fn();
  Rig rig(std::move(o));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  rig.router->OnGameClose(2);
  rig.router->OnGameOpen(3);
  QCHECK(rig.router->GetStats().heldRemotes == 1);
  {
    const auto held = rig.games.Holds();
    QCHECK(!held.empty() && held.back() == std::make_pair(GameId(3), true));
  }
  QCHECK(rig.games.Closes().empty());
  gate.Set(LoginGate::Ready);
  rig.router->ReevaluateHeldLogins();
  QCHECK(rig.remotes.Opens().size() == 2);
}

// Without a gate nothing is held, but the login connection is idle-exempt all the same.
void TestNoGateNeverHolds() {
  Rig rig;
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  QCHECK(rig.remotes.Opens().size() == 2 && rig.router->GetStats().heldRemotes == 0);
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
std::vector<RemoteOpenRequest> EstablishedForRemoval(Rig& rig) {
  OpenThree(rig);
  const auto opens = rig.remotes.Opens();
  rig.router->OnRemoteOpen(opens[0].remote);
  rig.router->OnRemoteOpen(opens[1].remote);
  rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLoginRequest, "GAME-LOGIN-" + kSecret), true);
  rig.router->OnRemoteFrame(opens[1].remote,
                            nevr_evr_codec::BuildLoginSuccess(kTestPlatform, kTestAccount) +
                                Msg(nevr_evr_codec::kSymConnectionUnrequire, "u"),
                            true);
  return opens;
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
  QCHECK(word10 == nevr_evr_codec::kLoginRemovedWord10);
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
  const auto opens = EstablishedForRemoval(rig);
  QCHECK(rig.router->GetStats().loginUserKnown);
  const RemoteId fresh = LoseAndReconnect(rig, opens[1].remote);
  QCHECK(fresh != kNoRemote);
  QCHECK(rig.router->GetStats().loginRemovedDue);
  QCHECK(rig.logs.Has("login-removed notice"));
  QCHECK(SentToGame(rig, 4).empty());  // nothing yet: the connection has not shown it is the login socket
  rig.router->OnRemoteOpen(fresh);
  rig.router->OnGameSilent(4);
  const auto toGame = SentToGame(rig, 4);
  QCHECK(toGame.size() == 1 && toGame[0] == ExpectedRemoved());
  QCHECK(rig.router->GetStats().loginsRemoved == 1);
  QCHECK(!rig.router->GetStats().loginRemovedDue);
  rig.router->OnGameSilent(4);  // idempotent
  QCHECK(CountNotices(rig) == 1);
  // The new session was never sent the game's old login or anything else of its own.
  for (const SentFrame& f : rig.remotes.Sent()) {
    if (f.id == fresh) QCHECK(f.data != Msg(nevr_evr_codec::kSymLoginRequest, "GAME-LOGIN-" + kSecret));
  }
  QCHECK(!rig.logs.Has(kSecret));
}

// Failure caught: the notice going to a socket that is not the login socket (or being sent twice per loss):
// a provisional connection that turns out to be config gets nothing, and the real (silent) login socket gets
// the one notice.
void TestOnlyAConnectionKnownToBeTheLoginSocketIsSentTheNotice() {
  Rig rig(RemovalOptions());
  const auto opens = EstablishedForRemoval(rig);
  const RemoteId provisional = LoseAndReconnect(rig, opens[1].remote);  // game 4: provisional login
  rig.router->OnRemoteOpen(provisional);
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymConfigRequest, "cfg"), true);  // it is the config socket
  QCHECK(SentToGame(rig, 4).empty());
  QCHECK(rig.router->GetStats().loginRemovedDue);  // still due
  rig.router->OnGameOpen(5);                       // the real login socket, silent
  rig.router->OnGameSilent(5);
  QCHECK(SentToGame(rig, 5).size() == 1 && SentToGame(rig, 5)[0] == ExpectedRemoved());
  QCHECK(CountNotices(rig) == 1);
  // A silent connection that is a matchmaker (not the login role) is never sent it.
  Rig rig2(RemovalOptions());
  const auto o2 = EstablishedForRemoval(rig2);
  const RemoteId r2 = LoseAndReconnect(rig2, o2[1].remote);
  rig2.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymFindSessionRequest, "find"), true);  // matchmaker
  rig2.router->OnGameSilent(4);
  (void)r2;
  QCHECK(SentToGame(rig2, 4).empty());
}

// Failure caught (the RETRY path): the login connection ended with a request outstanding. The game takes its
// own Lost path (-95, RETRY runs its login); a notice would only get in front of it.
void TestNoNoticeWhenTheLoginConnectionHadRequestsOutstanding() {
  Rig rig(RemovalOptions());
  const auto opens = EstablishedForRemoval(rig);
  rig.router->OnGameFrame(2, Msg(kSymSomething, "profile-request"), true);  // raises the count, no Unrequire
  const RemoteId fresh = LoseAndReconnect(rig, opens[1].remote);
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
  const auto opens = EstablishedForRemoval(rig);
  LoseAndReconnect(rig, opens[1].remote);
  rig.router->OnGameFrame(4, Msg(nevr_evr_codec::kSymLoginRequest, "OWN-" + kSecret), true);
  QCHECK(SentToGame(rig, 4).empty());
  QCHECK(!rig.router->GetStats().loginRemovedDue);
  QCHECK(rig.logs.Has("login removed notice skipped"));
  rig.router->OnGameSilent(4);  // ignored: it has spoken
  QCHECK(SentToGame(rig, 4).empty());
}

// Failure caught: a login-role request (no login) after the reconnect: the notice goes to that connection.
void TestLoginRoleRequestAfterTheReconnectGetsTheNotice() {
  Rig rig(RemovalOptions());
  const auto opens = EstablishedForRemoval(rig);
  LoseAndReconnect(rig, opens[1].remote);
  rig.router->OnGameFrame(4, Msg(kSymSomething, "profile-request"), true);
  const auto toGame = SentToGame(rig, 4);
  QCHECK(toGame.size() == 1 && toGame[0] == ExpectedRemoved());
}

// Failure caught: the account outliving the player's logout, a rejected login, or the router.
void TestTheAccountIdIsClearedByLogOutRejectionAndShutdown() {
  {  // dropped: the session already ended and the game's login socket has no live remote
    Rig rig(RemovalOptions());
    const auto opens = EstablishedForRemoval(rig);
    rig.router->OnRemoteError(opens[1].remote, 0, "network error");
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
    const auto opens = EstablishedForRemoval(rig);
    rig.router->OnGameFrame(2, Msg(nevr_evr_codec::kSymLogOut, "bye"), true);
    const RemoteId fresh = LoseAndReconnect(rig, opens[1].remote);
    rig.router->OnRemoteOpen(fresh);
    rig.router->OnGameSilent(4);
    QCHECK(SentToGame(rig, 4).empty());
  }
  {  // a rejected login
    Rig rig(RemovalOptions());
    OpenThree(rig);
    const auto opens = rig.remotes.Opens();
    rig.router->OnRemoteOpen(opens[1].remote);
    rig.router->OnRemoteFrame(opens[1].remote, nevr_evr_codec::BuildLoginSuccess(kTestPlatform, kTestAccount), true);
    QCHECK(rig.router->GetStats().loginUserKnown);
    rig.router->OnRemoteFrame(opens[1].remote, Msg(nevr_evr_codec::kSymLoginFailure, std::string(24, '\0')), true);
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
  const auto opens = EstablishedForRemoval(rig);
  QCHECK(!rig.router->GetStats().loginUserKnown);
  const RemoteId fresh = LoseAndReconnect(rig, opens[1].remote);
  rig.router->OnRemoteOpen(fresh);
  rig.router->OnGameSilent(4);
  QCHECK(CountNotices(rig) == 0);
  QCHECK(rig.router->GetStats().loginsRemoved == 0);
}

// One notice per lost session, and again after the next loss.
void TestOneNoticePerLostSession() {
  Rig rig(RemovalOptions());
  const auto opens = EstablishedForRemoval(rig);
  const RemoteId second = LoseAndReconnect(rig, opens[1].remote);
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
  TestConnectionIdentity();
  TestConfigRemoteEndDoesNotEndTheLoginSession();
  TestLoginIsFirstThenQueuedFramesInOrder();
  TestFrameDuringLoginBuildStaysBehindLogin();
  TestLoginInjectedOncePerLoginSessionOnly();
  TestNoLoginFrameWhenIdentityMissing();
  TestThrowingBuilderIsContained();
  TestRemoteCloseClosesGameSocketsWithoutTheLock();
  TestCloseMayReenterRouter();
  TestRemoteErrorBeforeOpenClosesGame();
  TestRemoteOpenRefusedClosesGame();
  TestGameCloseEndsItsPrivateRemoteButNotTheSharedLogin();
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
  TestOnlyAConnectionKnownToBeTheLoginSocketIsSentTheNotice();
  TestNoNoticeWhenTheLoginConnectionHadRequestsOutstanding();
  TestOwnLoginFirstGetsNoNotice();
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
  TestProvisionalLoginThatSendsConfigIsMoved();
  TestLoginConnectionWithSharersKeepsItsRole();
  TestLoginRequestOnAnotherConnectionTakesOverFromALiveLogin();
  TestHeldLoginOpensWhenTheAccountAppears();
  TestConfigConnectionStillFailsFastWhileTheLoginIsHeld();
  TestHeldLoginIsClosedWhenTheAccountIsRefused();
  TestReplacementLoginConnectionIsHeldOnTheSameSession();
  TestNoGateNeverHolds();
  TestOnlyTheLoginConnectionIsIdleExempt();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "session_router_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("session_router_test: all checks passed\n");
  return 0;
}
