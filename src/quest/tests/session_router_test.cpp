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

using namespace SessionRouter;

namespace {

constexpr uint64_t kSymSomething = 0x1111222233334444ULL;  // an arbitrary game message
const std::string kSecret = "SECRET-TOKEN-VALUE";

std::string Msg(uint64_t symbol, const std::string& payload = "x") { return EvrCodec::BuildMessage(symbol, payload); }

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

const std::string kLogin = Msg(EvrCodec::kSymLoginRequest, "LOGIN-PAYLOAD-" + kSecret);

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
  for (const auto& s : sent) QCHECK(EvrCodec::FirstSymbol(s.data) != EvrCodec::kSymLoginRequest);
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
  Rig rig(WithLogin(kLogin));
  rig.router->OnGameOpen(1);
  rig.router->OnGameOpen(2);
  const RemoteId login = rig.remotes.Opens()[1].remote;
  rig.router->OnRemoteOpen(login);
  rig.router->OnRemoteFrame(login, Msg(EvrCodec::kSymLoginSuccess, std::string(32, '\0')), true);
  QCHECK(rig.games.Sent().size() == 1);
  QCHECK(rig.logs.Has("LOGIN SUCCESS"));
  const auto sent = rig.remotes.Sent();
  QCHECK(sent.size() == 2);  // login, subscribe
  if (sent.size() == 2) QCHECK(sent[1].data == EvrCodec::BuildFriendListSubscribe());

  std::string failurePayload;
  EvrCodec::AppendLE64(failurePayload, 0);   // placeholder fields
  EvrCodec::AppendLE64(failurePayload, 0);
  EvrCodec::AppendLE64(failurePayload, 7);   // status
  failurePayload += "PRIVATE-SERVER-TEXT";
  rig.router->OnRemoteFrame(login, Msg(EvrCodec::kSymLoginFailure, failurePayload), true);
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
    const uint64_t sym = EvrCodec::FirstSymbol(s.data);
    if (sym != 2 && sym != 3) continue;
    const std::string payload = s.data.substr(EvrCodec::kHeaderSize);
    int& next = sym == 2 ? next2 : next3;
    if (payload != std::to_string(next)) ordered = false;
    ++next;
  }
  QCHECK(ordered);
  QCHECK(next2 == kPerThread && next3 == kPerThread);
  QCHECK(rig.games.Sent().size() == static_cast<std::size_t>(kPerThread));
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

}  // namespace

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
  TestShutdown();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "session_router_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("session_router_test: all checks passed\n");
  return 0;
}
