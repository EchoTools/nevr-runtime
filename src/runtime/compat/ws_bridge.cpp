#include "runtime/compat/ws_bridge.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/hmd_serial.h"
#include "runtime/compat/lobby_frame.h"
#include "runtime/compat/login_profile.h"
#include "runtime/compat/social_names.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"
#include "runtime/compat/socket_role.h"
#include "runtime/compat/session_router.h"
#include "runtime/compat/session_router_mode.h"
#include "runtime/compat/ws_session_transport.h"
#include "runtime/hook/symbol_corpus.h"
#include "runtime/patch/asset_registry.h"
#include "runtime/patch/provider_identity.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <cstring>
#include <ctime>
#include <memory>
#include <limits>
#include <mutex>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>

#include "runtime/lifecycle/config.h"
#include "runtime/ext/module_loader.h"
#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "core/system_info.h"
#include "core/build_identity.h"   // N112: NEVR version identity
#include "runtime/ext/plugin_loader.h"  // N112: plugin manifest
#include "runtime/lifecycle/cli.h"  // g_isServer
#include "runtime/lifecycle/service_config.h"  // NevrCfgGetFlat (N133 S4a: config.yaml reads)
#include "runtime/lifecycle/crash_recovery.h"  // ServerFatal (#1254: a refused server login stops)
#include "runtime/compat/login_refusal.h"
#include "runtime/log/url_diagnostics.h"
#include "runtime/log/security_diagnostics.h"
#include "runtime/server/serverdb_uri.h"
#include "runtime/compat/friend_request_check.h"
#include "runtime/compat/party_leave_check.h"
#include "runtime/compat/party_share_check.h"
#include "runtime/compat/self_check.h"
#include "core/nevr_root.h"
#include "runtime/compat/credential_cache_check.h"
#include "runtime/compat/status_redirect_check.h"
#include "core/logging.h"
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <stdexcept>
#include <utility>

// ============================================================================
// N85 — exception boundary for ixwebsocket callbacks
// ============================================================================
//
// These lambdas are invoked BY ixwebsocket, on ixwebsocket's own threads. That
// makes each one a DLL/library boundary, and the CPP addendum's rule applies:
// never let an exception unwind across it.
//
// Measured consequence of not doing so: a C++ throw escaping one of these
// callbacks reaches the game's top-level unhandled-exception filter
// (0x1401CEE70 -> HandleCrashDump -> WriteCrashSystemInfo, which is what prints
// "=== System Info ==="), and the server dies. Exception code 0x20474343 is
// 'GCC ' — the MinGW throw magic. The game is MSVC-built and cannot raise it,
// so any 0x20474343 in a server log came from a NEVR DLL.
//
// std::exception is named explicitly rather than catch(...) per the addendum.
// A non-std::exception throw would still escape, and that is deliberate: it
// would indicate something we do not model, and should be visible.
template <typename Fn>
static auto GuardWsCallback(const char* what, Fn&& fn) {
  return [what, fn = std::forward<Fn>(fn)](auto&&... args) {
    try {
      fn(std::forward<decltype(args)>(args)...);
    } catch (const std::exception&) {
      const std::string diagnostic = nevr_log_diagnostics::FormatCallbackFailureDiagnostic(what);
      Log(EchoVR::LogLevel::Error, "%s", diagnostic.c_str());
    }
  };
}

namespace {
// A LoginFailure payload: [session UUID(16)][status code(8)], then the NUL-terminated message text.
constexpr size_t kLoginFailureFixedPayloadSize = 24;

// The server's NewLocationError (nakama server/evr_pipeline_login.go) puts the line
// "Select code >>> NN <<<" last; the game's login-failure screen shows only the first lines, so the
// player never sees the code (#201). When the frame is exactly one LoginFailure whose text has such a
// line, return the frame with that line moved to the front and the rest of the text after it. Any other
// frame or text returns nullopt and is forwarded byte-identical.
std::optional<std::string> MoveCodeLineFirst(const std::string& frame) {
  if (frame.size() < nevr_evr_codec::kHeaderSize + kLoginFailureFixedPayloadSize + 1) return std::nullopt;
  uint64_t symbol = 0;
  uint64_t payloadLength = 0;
  memcpy(&symbol, frame.data() + 8, sizeof(symbol));
  memcpy(&payloadLength, frame.data() + 16, sizeof(payloadLength));
  if (symbol != nevr_evr_codec::kSymLoginFailure || payloadLength != frame.size() - nevr_evr_codec::kHeaderSize) return std::nullopt;
  if (frame.back() != '\0') return std::nullopt;

  const size_t textStart = nevr_evr_codec::kHeaderSize + kLoginFailureFixedPayloadSize;
  const std::string text = frame.substr(textStart, frame.size() - 1 - textStart);
  if (text.find('\0') != std::string::npos) return std::nullopt;

  std::vector<std::string> lines;
  size_t begin = 0;
  while (true) {
    const size_t end = text.find('\n', begin);
    lines.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  static const std::string kPrefix = "Select code >>> ";
  static const std::string kSuffix = " <<<";
  size_t codeLine = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (line.size() <= kPrefix.size() + kSuffix.size() || line.compare(0, kPrefix.size(), kPrefix) != 0 ||
        line.compare(line.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
      continue;
    }
    const std::string code = line.substr(kPrefix.size(), line.size() - kPrefix.size() - kSuffix.size());
    if (!code.empty() && code.find_first_not_of("0123456789") == std::string::npos) codeLine = i;
  }
  if (codeLine == lines.size() || codeLine == 0) return std::nullopt;

  std::string reordered = lines[codeLine];
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i == codeLine) continue;
    reordered += '\n';
    reordered += lines[i];
  }
  std::string out = frame.substr(0, textStart);
  out += reordered;
  out.push_back('\0');
  const uint64_t newLength = out.size() - nevr_evr_codec::kHeaderSize;
  memcpy(&out[16], &newLength, sizeof(newLength));
  return out;
}
}  // namespace

// ============================================================================
// In-process WebSocket TLS proxy
// ============================================================================
//
// The game's CWebSocket uses Schannel/Wine GnuTLS which fails with TLS
// handshake errors when connecting to echovrce.com. Rather than hooking
// the complex CWebSocket internals, we run an in-process ws:// server
// that the game connects to natively. The server proxies each connection
// to the real wss:// endpoint via ixwebsocket (which uses mbedTLS).
//
// Config: nevr_socket_uri = "wss://g.echovrce.com/ws"
// RedirectServiceUrl rewrites this to "ws://localhost:PORT" when the proxy is active.
// The game's CWebSocket connects to the local server — no TLS needed.

// The holders below have no static destructor: DLL_PROCESS_DETACH runs under the
// loader lock, where a WebSocketServer destructor could join its worker thread.
// The graceful path explicitly stops and resets both listeners; on process exit
// the OS reclaims any sockets still held by these leaked holders.
static std::unique_ptr<ix::WebSocketServer>& g_server = *new std::unique_ptr<ix::WebSocketServer>();
static std::unique_ptr<ix::WebSocketServer>& g_matchServer = *new std::unique_ptr<ix::WebSocketServer>();
static std::string g_remoteUri;
static uint16_t g_proxyPort = 0;
static bool g_bridgeEnabled = false;
static uint16_t g_matchPort = 0;

// Per-connection state: maps game-side server WebSocket → remote ix::WebSocket
struct ProxyPair {
  std::shared_ptr<ix::WebSocket> remoteWs;
  std::vector<std::string> pendingToRemote;
  bool remoteOpen = false;
  bool loginInjected = false;  // true after we inject LoginRequest on this connection
  int connIdx = -1;  // 0=config, 1=login, >=2=shares the login session (see nevr_socket_role::RoleTable)
  nevr_socket_role::Role role = nevr_socket_role::Role::Config;
  nevr_socket_role::Listener listener = nevr_socket_role::Listener::Service;
  bool firstFrameChecked = false;  // the first frame the game sent has been compared with `role`
  std::weak_ptr<ix::WebSocket> gameWs;  // the game socket this pair serves; owned by the game server's client set
};

// Human-readable label for connIdx, for log lines — this is the ONLY place
// that tells "connection ... closed" apart across config/login/matchmaker;
// without it every closed-connection log line is ambiguous (all three
// connections share one local bridge port per service, see RedirectServiceUrl).
static const char* ConnLabel(int connIdx) {
  switch (connIdx) {
    case 0:  return "config";
    case 1:  return "login";
    default: return connIdx >= 2 ? "matchmaker" : "unknown";
  }
}

// Name of a remote websocket's ready state, for log lines.
static const char* RemoteStateName(ix::ReadyState state) {
  switch (state) {
    case ix::ReadyState::Connecting: return "connecting";
    case ix::ReadyState::Open: return "open";
    case ix::ReadyState::Closing: return "closing";
    case ix::ReadyState::Closed: return "closed";
  }
  return "unknown";
}

static std::mutex g_pairsMutex;
// Which slot each accepted game socket takes. Guarded by g_pairsMutex (RoleTable has no lock of its own).
static nevr_socket_role::RoleTable g_roles;
static auto& g_pairs = *new std::unordered_map<ix::WebSocket*, std::unique_ptr<ProxyPair>>();

// Defined after the login-session globals below.
static bool ForgetLoginSessionLocked(const ix::WebSocket* remote, std::shared_ptr<ix::WebSocket>* ended);

// Drops one reference to an ix::WebSocket on a thread of its own (#751). ~WebSocket calls stop(), which joins the
// socket's thread: released on that thread it joins itself, and released under g_pairsMutex it waits for threads
// that are blocked on the mutex. Only the handoff happens at the call site; the destructor runs on the new thread.
static void ReleaseOffThread(std::shared_ptr<ix::WebSocket> ws) {
  if (!ws) return;
  std::thread([last = std::move(ws)]() {}).detach();
}

// The game sockets whose pair forwards to `remote` (the login socket and any matchmaker sockets sharing
// its session). Caller holds g_pairsMutex.
static std::vector<const ix::WebSocket*> GameSocketsBoundToLocked(const ix::WebSocket* remote) {
  std::vector<const ix::WebSocket*> games;
  for (const auto& entry : g_pairs) {
    if (entry.second && entry.second->remoteWs.get() == remote) games.push_back(entry.first);
  }
  return games;
}

// True on a thread that is inside a remote's own close callback ending that remote's session. The session-end
// self-check flush runs there and would send on the remote that is closing. On an abnormal close (the TCP
// connection dropped, code 1006) ix::WebSocket calls the close callback with its state still OPEN and its
// non-recursive _setReadyStateMutex held; sendBinary on the dead socket fails in sendOnSocket and calls
// setReadyState, which takes that mutex again. The remote's thread blocked on itself, and graceful shutdown
// (WebSocket::stop joins that thread) never finished (#723). Nothing can be delivered on a closed remote;
// SendFrameToServer refuses while this is set, and the self-check counts the undelivered results as lost when
// the session ends ("unsent when a session ended").
static thread_local bool t_remoteSessionEnding = false;
struct RemoteSessionEndingScope {
  RemoteSessionEndingScope() { t_remoteSessionEnding = true; }
  ~RemoteSessionEndingScope() { t_remoteSessionEnding = false; }
  RemoteSessionEndingScope(const RemoteSessionEndingScope&) = delete;
  RemoteSessionEndingScope& operator=(const RemoteSessionEndingScope&) = delete;
};

// A remote session that ends must end the game's sockets on it too, or the game sits "logged in" with no
// server and never reconnects (#70: forced cut, run 20-1dd53339e6a29b8). Called from a remote callback.
// Closing a game socket inline deadlocks (close() waits on the server thread, which may wait on
// g_pairsMutex), so the close runs on a thread that holds no lock, and each socket is kept alive by the
// server's own shared_ptr (WebSocketServer::getClients) rather than a raw pointer.
static void CloseGameSocketsForRemote(const ix::WebSocket* remote, int connIdx, unsigned int code) {
  std::vector<const ix::WebSocket*> games;
  bool wasLoginSession = false;
  std::shared_ptr<ix::WebSocket> endedLogin;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    games = GameSocketsBoundToLocked(remote);
    wasLoginSession = ForgetLoginSessionLocked(remote, &endedLogin);
  }
  // This is the remote's own callback thread and the mutex is free: the last reference, if this was it, dies elsewhere.
  ReleaseOffThread(std::move(endedLogin));
  if (wasLoginSession) {
    RemoteSessionEndingScope ending;  // this is the closing remote's own callback thread: send nothing on it
    nevr_self_check::SetLoggedIn(false);  // no result is sent until the next LoginSuccess; outside g_pairsMutex
    Log(EchoVR::LogLevel::Info,
        "[NEVR.WS] login session ended: the game's next login socket is a new login (shared connections renumbered from 2)");
  }
  if (games.empty() || !g_server) {
    Log(EchoVR::LogLevel::Info, "[NEVR.WS] remote session ended (conn=%d, %s, code=%u): no game socket on it",
        connIdx, ConnLabel(connIdx), code);
    return;
  }
  std::vector<std::shared_ptr<ix::WebSocket>> toClose;
  for (const auto& client : g_server->getClients()) {
    if (std::find(games.begin(), games.end(), client.get()) != games.end()) toClose.push_back(client);
  }
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.WS] remote session ended (conn=%d, %s, code=%u): closing %zu game socket(s) so the game "
      "reconnects",
      connIdx, ConnLabel(connIdx), code, toClose.size());
  std::thread([toClose]() {
    for (const auto& game : toClose) game->close();
  }).detach();
}

// #1254: no remote can be made for a game socket that just opened (a server without a credential, or a sign-in the
// service refused). The socket is ended so the game sees the login fail rather than wait on a session that will never
// open, and the login slot it took (admission) is given back so the next /login is a login, not a share of nothing.
// Closed off this thread for the reason CloseGameSocketsForRemote gives: close() waits on the server thread.
static void AbandonGameSocket(ix::WebSocket* gameWs, nevr_socket_role::Role role, int connIdx) {
  std::shared_ptr<ix::WebSocket> keep;
  for (ix::WebSocketServer* server : {g_server.get(), g_matchServer.get()}) {
    if (server == nullptr) continue;
    for (const auto& client : server->getClients()) {
      if (client.get() == gameWs) keep = client;
    }
  }
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    if (role == nevr_socket_role::Role::Login) g_roles.LoginSessionEnded();
  }
  Log(EchoVR::LogLevel::Debug, "[NEVR.WS] conn=%d (%s): no remote for this game socket; closing it", connIdx,
      ConnLabel(connIdx));
  if (keep) std::thread([keep]() { keep->close(); }).detach();
}

// Connection index of a game-side websocket (-1 when unknown), for log lines.
static int ConnIdxOfGameWs(ix::WebSocket* gameWs) {
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  const auto it = g_pairs.find(gameWs);
  return it == g_pairs.end() ? -1 : it->second->connIdx;
}

// The login connection's remote WS (conn=1). Connections after login (conn>=2,
// e.g. matchmaker) reuse this so all traffic shares the same Nakama session.
// The original game multiplexes config/login/matchmaker on one WS to one server;
// Nakama correlates matchmaker allocations by session, so the matchmaker must
// use the same authenticated session as login.
static std::shared_ptr<ix::WebSocket>& g_loginRemoteWs = *new std::shared_ptr<ix::WebSocket>();

// The active game-side WS that should receive server→game messages from the
// login remote. Initially conn=1 (login), swapped to conn=2 (matchmaker) when
// it connects, so the game receives matchmaker responses on the right peer.
static ix::WebSocket* g_activeGameWs = nullptr;

// N91: one login per login session. `loginInjected` lives on the ProxyPair, so a check
// per connection passes on every connection and each one injects a LoginRequest: two
// logins, two OCS sessions. The guard is the login slot in g_roles: only the socket that
// takes it (nevr_socket_role::RoleTable, Role::Login) injects, a further socket that asks
// for /login while the slot is taken shares the session without injecting, and the slot
// is freed only when the login session ends (ForgetLoginSessionLocked). Nothing injects
// on the config socket.

// Account id used by the last injection — the server-mode fake LoginSuccess (N92)
// echoes it back so the game sees a consistent identity.
static uint64_t g_lastInjectedDiscordId = 0;

// The game-side WS that registered the shared remote's onMessageCallback
// (conn=1 — login). When this connection closes, the lambda's captured pointers
// (pairPtr, gameWsPtr) become dangling. Used in the Close handler to clear the
// callback only when the owning pair is destroyed, not when a sharing pair
// (conn>=2, matchmaker) closes.
static ix::WebSocket* g_loginGameWs = nullptr;

// Each game socket is config (conn 0), login (conn 1) or shares the login session (conn 2 and up, the
// matchmaker). The role comes from what the socket asked for, not from when it connected: see
// socket_role.h. When the login session itself ends, the game reconnects to log in again; without
// forgetting the dead session, that connection is taken for one more socket on it and attached to it (#70,
// run 20-1dd5334c6f870d2 is the measurement that showed it). Caller holds g_pairsMutex. True when `remote`
// was the login session. The caller ends the self-check session (nevr_self_check::SetLoggedIn(false)) AFTER it
// releases the mutex: that call flushes the queued results through SendFrameToServer, which takes g_pairsMutex
// itself, so calling it here deadlocked the remote's thread on its own lock and made WebSocket::stop() wait on
// that thread forever (#723, #729). The session's reference moves into `ended`, which the caller releases through
// ReleaseOffThread after it releases the mutex: dropping it here could run ~WebSocket under the lock (#751).
static bool ForgetLoginSessionLocked(const ix::WebSocket* remote, std::shared_ptr<ix::WebSocket>* ended) {
  if (remote == nullptr || remote != g_loginRemoteWs.get()) return false;
  *ended = std::move(g_loginRemoteWs);
  g_loginGameWs = nullptr;
  g_roles.LoginSessionEnded();
  return true;
}

// Under g_pairsMutex. The game socket that frames from the shared login remote go to: the active one
// (the newest connection sharing the remote), else the login connection; nullptr when neither is
// still connected. The remote's message callback belongs to whichever connection opened last, and
// that connection may have closed since (conn=3 closes after the lobby join), so the target is looked
// up for every frame, never captured. Routing to the captured socket sent every reply after that
// close to a dead connection: the game never saw its profile replies again (2026-10-01).
static ProxyPair* SharedRouteLocked(ix::WebSocket** target) {
  for (ix::WebSocket* ws : {g_activeGameWs, g_loginGameWs}) {
    if (ws == nullptr) continue;
    const auto it = g_pairs.find(ws);
    if (it != g_pairs.end()) {
      *target = ws;
      return it->second.get();
    }
  }
  *target = nullptr;
  return nullptr;
}

// Under g_pairsMutex: the game closed `gameWs`. Clears the remote's callback where nothing else uses
// that remote, forgets the pair, and moves server->game routing to the newest connection still
// sharing the login remote. Returns the remote to stop OUTSIDE the lock (N60), for an unshared pair.
static std::shared_ptr<ix::WebSocket> RetireGameWsLocked(ix::WebSocket* gameWs, int* closedConnIdx,
                                                         bool* callbackCleared) {
  std::shared_ptr<ix::WebSocket> remoteToStop;
  auto it = g_pairs.find(gameWs);
  if (it != g_pairs.end()) {
    *closedConnIdx = it->second->connIdx;
    const bool isShared = (it->second->remoteWs == g_loginRemoteWs);
    if (!isShared) {
      remoteToStop = it->second->remoteWs;
      // N85: a no-op, NOT nullptr. ixwebsocket invokes _onMessageCallback unconditionally; an empty
      // std::function throws std::bad_function_call out of ixwebsocket's thread and kills the
      // dedicated server (confirmed from a crash-dump stack).
      it->second->remoteWs->setOnMessageCallback([](const ix::WebSocketMessagePtr&) {});
      *callbackCleared = true;
    } else if (gameWs == g_loginGameWs) {
      // N61: the login pair shares the remote with the matchmaker connections; clear the callback
      // only if none of them is active, or matchmaker routing dies with it.
      if (g_activeGameWs == nullptr || g_activeGameWs == gameWs) {
        it->second->remoteWs->setOnMessageCallback([](const ix::WebSocketMessagePtr&) {});
        *callbackCleared = true;
      }
    }
    g_pairs.erase(it);
    if (gameWs == g_loginGameWs) g_loginGameWs = nullptr;  // the login slot never keeps a retired socket
  }
  if (g_activeGameWs == gameWs) {
    g_activeGameWs = nullptr;
    int newest = -1;
    for (const auto& entry : g_pairs) {
      if (entry.first != g_loginGameWs && entry.second->remoteWs == g_loginRemoteWs && entry.second->connIdx > newest) {
        newest = entry.second->connIdx;
        g_activeGameWs = entry.first;
      }
    }
    ix::WebSocket* target = nullptr;
    const ProxyPair* route = SharedRouteLocked(&target);
    Log(EchoVR::LogLevel::Info, "[NEVR.WS] server->game routing: conn=%d closed, frames from the login session now go to conn=%d",
        *closedConnIdx, route != nullptr ? route->connIdx : -1);
  }
  return remoteToStop;
}

// Under g_pairsMutex. The first frame a game socket sends names its role (SNSConfigRequestv2 is config, the
// lobby requests are matchmaker, LogInRequestv2 is login: nevr_socket_role::RoleOfFirstSymbol), the check
// ADR 0003 makes on Quest. A socket whose frame contradicts the role it was admitted with is logged once,
// with both roles: the roles come from the request target, so a contradiction means the game used a
// service's socket for another service, and the stall that follows is then named in the log. Only sockets on
// the service listener are checked: the matchmaker listener carries the matchmaker's protobuf, not EVR frames.
static void CheckFirstFrameRoleLocked(ProxyPair& pair, const std::string& frame) {
  if (pair.firstFrameChecked) return;
  pair.firstFrameChecked = true;
  if (pair.listener != nevr_socket_role::Listener::Service) return;
  const uint64_t symbol = nevr_evr_codec::FirstSymbol(frame);
  const std::optional<nevr_socket_role::Role> named = nevr_socket_role::RoleOfFirstSymbol(symbol);
  if (!named || *named == pair.role) return;
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.WS] first frame contradicts the socket's role conn=%d role=%s first_frame_role=%s sym=0x%016llx: "
      "the game used this socket for another service, so its requests are served as %s",
      pair.connIdx, nevr_socket_role::RoleName(pair.role), nevr_socket_role::RoleName(*named),
      static_cast<unsigned long long>(symbol), nevr_socket_role::RoleName(pair.role));
}

// A frame from the shared login remote with no live game connection to take it.
static void LogSharedFrameDropped(std::size_t bytes) {
  static std::atomic<std::uint64_t> s_dropped{0};
  const std::uint64_t dropped = ++s_dropped;
  if (dropped == 1 || dropped % 100 == 0) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.WS] server->game frame DROPPED: no game connection is still open on the login session bytes=%zu "
        "(%llu total)", bytes, static_cast<unsigned long long>(dropped));
  }
}

// ============================================================================
// LoginRequest builder
// ============================================================================
// EchoVR wire format: [marker(8)][symbol(8)][length(8)][payload]
// LoginRequest payload: [UUID(16)][PlatformCode(8)][AccountId(8)][JSON\0]

// Every message in a bridged frame, by name, in arrival order. Nakama batches messages into one
// frame (LoginSuccess, STcpConnectionUnrequireEvent and GameSettings arrive together), so logging
// only the first symbol hid the rest. Every message is a per-frame record, so it logs at Debug on a
// server as on a client (docs/standards/logging.md, "You SHALL NOT": no INFO on a per-connection path).
static int LogFrameMessages(const char* direction, int connIdx, const std::string& frame) {
  size_t offset = 0;
  int count = 0;
  nevr_evr_codec::Message message;
  while (true) {
    const nevr_evr_codec::ReadStatus status = nevr_evr_codec::ReadMessage(frame, offset, &message);
    if (status == nevr_evr_codec::ReadStatus::End || status == nevr_evr_codec::ReadStatus::BadMarker) break;
    const char* name = EchoVR::LookupSymbolName(message.symbol);
    Log(EchoVR::LogLevel::Debug, "[NEVR.WS] %s conn=%d (%s) msg=%d sym=0x%016llx %s len=%llu", direction, connIdx,
        ConnLabel(connIdx), count, static_cast<unsigned long long>(message.symbol), name ? name : "<unnamed>",
        static_cast<unsigned long long>(message.length));
    ++count;
    if (status == nevr_evr_codec::ReadStatus::Truncated) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.WS] %s conn=%d msg=%d declares %llu bytes but %zu remain",
          direction, connIdx, count - 1, static_cast<unsigned long long>(message.length),
          frame.size() - offset - nevr_evr_codec::kHeaderSize);
      break;
    }
    offset += nevr_evr_codec::kHeaderSize + static_cast<size_t>(message.length);
  }
  const size_t remaining = frame.size() - offset;
  if (remaining > 0) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.WS] %s conn=%d %zu bytes not in message framing", direction, connIdx,
        remaining);
  }
  return count;
}

// Debug-level decode of every message the game sends to the server, for diagnostics only: the raw
// frame is forwarded whether or not this walk finishes. A message whose declared payload runs past the
// end of the frame stops the walk BEFORE any decoder reads its payload. Returns the messages walked.
static int LogGameToServerFrame(const std::string& frame, const std::string& wsConnId) {
  size_t offset = 0;
  int msgIdx = 0;
  nevr_evr_codec::Message message;
  while (true) {
    const nevr_evr_codec::ReadStatus status = nevr_evr_codec::ReadMessage(frame, offset, &message);
    if (status == nevr_evr_codec::ReadStatus::End) break;
    if (status == nevr_evr_codec::ReadStatus::BadMarker) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.WS] game->server: bad marker at offset %zu — per-message diagnostic "
          "decode aborted here, raw frame still forwarded to remote unparsed",
          offset);
      break;
    }
    const uint64_t sym = message.symbol;
    const uint64_t len = message.length;
    if (status == nevr_evr_codec::ReadStatus::Truncated) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.WS]   truncated: header declares %llu payload bytes but only %zu remain — per-message "
          "diagnostic decode aborted here, raw frame still forwarded to remote unparsed",
          static_cast<unsigned long long>(len), frame.size() - offset - nevr_evr_codec::kHeaderSize);
      break;
    }
    // From here the message lies wholly inside the frame: message.payload..+len is readable (the codec
    // sets payload only for such a message).
    const uint8_t* payload = message.payload;
    char symBuf[192];
    const char* symName = EchoVR::LookupSymbolName(sym);
    if (symName) {
      snprintf(symBuf, sizeof(symBuf), "0x%016llx (%s)", static_cast<unsigned long long>(sym), symName);
    } else {
      snprintf(symBuf, sizeof(symBuf), "0x%016llx", static_cast<unsigned long long>(sym));
    }
    Log(EchoVR::LogLevel::Debug, "[NEVR.WS] game->server [%d]: sym=%s len=%llu ws_conn_id=%s", msgIdx, symBuf,
        static_cast<unsigned long long>(len), wsConnId.c_str());
    // Hex dump PlayerSessionRequest (0x9af2fab2a0c81a05) for debugging
    if (sym == 0x9af2fab2a0c81a05 && len <= 256) {
      char hex[1024] = {};
      int hoff = 0;
      for (size_t i = 0; i < len && hoff < 1000; i++) {
        hoff += snprintf(hex + hoff, sizeof(hex) - hoff, "%02x ", payload[i]);
      }
      Log(EchoVR::LogLevel::Debug, "[NEVR.WS] PlayerSessionReq payload: %s", hex);
    }
    // Decode outgoing SNS friend messages
    // FriendInviteRequest (0x7f0d7a28de3c6f70): RoutingID(8)+UUID(16)+SessionGUID(8)+TargetUserID(8)
    if (sym == 0x7f0d7a28de3c6f70 && len >= 0x28) {
      uint64_t routingId, sessionGuid, targetUserId;
      memcpy(&routingId, payload, 8);
      memcpy(&sessionGuid, payload + 24, 8);
      memcpy(&targetUserId, payload + 32, 8);
      Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   FriendInvite: routing=%llu target=%llu session=%llu",
          static_cast<unsigned long long>(routingId), static_cast<unsigned long long>(targetUserId),
          static_cast<unsigned long long>(sessionGuid));
    }
    // SNSPartyInviteRequest (0xcf13f934540b5f5e): RoutingID(8)+UUID(16)+SessionGUID(8)+TargetUserID(8)
    // (Debug, alongside the other per-message decodes in this loop.)
    if (sym == 0xcf13f934540b5f5e && len >= 0x28) {
      uint64_t routingId, sessionGuid, targetUserId;
      memcpy(&routingId, payload, 8);
      memcpy(&sessionGuid, payload + 24, 8);
      memcpy(&targetUserId, payload + 32, 8);
      Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   PartyInviteRequest: routing=%llu target=%llu session=%llu",
          static_cast<unsigned long long>(routingId), static_cast<unsigned long long>(targetUserId),
          static_cast<unsigned long long>(sessionGuid));
    }
    // FriendListSubscribe (0xdcfa94680e8d19fc)
    if (sym == 0xdcfa94680e8d19fc) {
      Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   FriendListSubscribeRequest sent");
    }
    offset += nevr_evr_codec::kHeaderSize + static_cast<size_t>(len);
    msgIdx++;
  }
  if (frame.size() - offset > 0 && msgIdx > 0) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   %zu trailing bytes after %d messages", frame.size() - offset, msgIdx);
  }
  return msgIdx;
}

// #929: a client's Info record of each lobby find and join request the game sends and each session success and
// failure the service answers with, on a server and a client alike. The fields are the operation, the lobby ids (GUIDs,
// never names), the entrants as XPIDs and the failure code; the settings JSON of a request is remote text and is not read
// out, and a failure's message is counted (message_bytes) and never copied (logging.md Rule 4). A failure frame
// carries no lobby id, so a session record names the last request the game sent on this client, on any connection
// (last_request), and the party it is in (party_id), not a request or a party it could pair with on its own conn
// (#1413).
static uint64_t RealLobbyClockMs() { return GetTickCount64(); }
static std::mutex g_lobbyLimiterMutex;
// The limiter per kind and the request tracker (#1413). The records' text is lobby_frame.h's, shared with the Quest router.
static nevr_lobby_frame::ClientLobbyRecords g_lobbyRecords;
// The lobby this client is in and the one it is joining, set from the same frames (#1413, compat/lobby_join_state.h).
static nevr_lobby_join_state::LobbyJoinState g_lobbyJoin;
static uint64_t (*g_lobbyClockMs)() = &RealLobbyClockMs;
// The party a session record names: the process-wide party state, read when the record is written (#1413).
static nevr_social_party::View RealLobbyPartyView() { return nevr_social_party::Global().Snapshot(); }
static nevr_social_party::View (*g_lobbyPartyView)() = &RealLobbyPartyView;

void LobbyJoinSnapshot(nevr_lobby_join_state::Stamp* current, nevr_lobby_join_state::Stamp* joining) {
  g_lobbyJoin.Snapshot(current, joining);
}

// The party of a session record: the party the view names when the record is written (#1413).
static std::optional<nevr_lobby_frame::PartyContext> LobbyParty(nevr_social_party::View (*partyView)()) {
  const nevr_social_party::View view = partyView();
  return nevr_lobby_frame::PartyContext{view.partyId, static_cast<uint64_t>(view.members.size()), view.ownerId,
                                        view.selfId};
}

static void LogClientLobbyMessage(int connIdx, bool fromServer, uint64_t symbol, const uint8_t* payload, uint64_t len) {
  const std::optional<nevr_lobby_frame::Kind> kind = nevr_lobby_frame::KindOf(symbol, fromServer);
  if (!kind) return;
  // The game's party-follow loop can send a request every update, so a kind writes a burst per window and the
  // rest are counted; an ended window's count is written by the next lobby frame of any kind, before its record.
  // The request tracker takes every request and every answer, also those the limiter suppresses, so an answer takes
  // the request it answers whether or not its record is written (#1413).
  nevr_lobby_frame::RecordLimiter::Verdict verdict;
  nevr_lobby_frame::RequestContext request;
  nevr_social_party::View (*partyView)() = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lobbyLimiterMutex);
    const nevr_lobby_frame::ClientLobbyRecords::Admission admission = g_lobbyRecords.Admit(*kind, g_lobbyClockMs());
    verdict = admission.verdict;
    request = admission.request;
    partyView = g_lobbyPartyView;
  }
  // The join state takes every decoded frame, not only the ones the limiter writes: a suppressed record still
  // changed what the game is joining or in (#1413).
  nevr_lobby_frame::Decoded state;
  if (nevr_lobby_frame::Decode(*kind, payload, static_cast<size_t>(len), &state)) {
    switch (*kind) {
      case nevr_lobby_frame::Kind::Join:
        g_lobbyJoin.OnJoinRequested(state.lobby);
        break;
      case nevr_lobby_frame::Kind::Success:
        g_lobbyJoin.OnSuccess(state.lobby);
        break;
      case nevr_lobby_frame::Kind::Failure:
        g_lobbyJoin.OnFailure(request.sent && request.kind == nevr_lobby_frame::Kind::Join);
        break;
      case nevr_lobby_frame::Kind::Find:
        break;
    }
  }
  for (size_t i = 0; i < nevr_lobby_frame::kKindCount; ++i) {
    if (verdict.flushed[i] == 0) continue;
    Log(EchoVR::LogLevel::Info, "%s",
        nevr_lobby_frame::FormatSuppressedNotice(static_cast<nevr_lobby_frame::Kind>(i), verdict.flushed[i]).c_str());
  }
  if (!verdict.log) return;
  nevr_lobby_frame::Decoded d;
  if (!nevr_lobby_frame::Decode(*kind, payload, static_cast<size_t>(len), &d)) {
    Log(EchoVR::LogLevel::Warning, "%s", nevr_lobby_frame::FormatUnreadable(*kind, connIdx, len).c_str());
    return;
  }
  switch (d.kind) {
    case nevr_lobby_frame::Kind::Find: {
      std::string mode;
      if (const char* modeName = EchoVR::LookupSymbolName(d.mode); modeName != nullptr) {
        mode = modeName;
      } else {
        char hex[24];
        std::snprintf(hex, sizeof(hex), "0x%016llx", static_cast<unsigned long long>(d.mode));
        mode = hex;
      }
      Log(EchoVR::LogLevel::Info,
          "[NEVR.WS] lobby find requested conn=%d mode=%s entrants=%zu accounts=%s current_lobby=%s group=%s", connIdx,
          mode.c_str(), d.entrants.size(), nevr_lobby_frame::FormatAccounts(d.entrants).c_str(),
          nevr_lobby_frame::FormatGuidOrNone(d.currentLobby).c_str(), nevr_lobby_frame::FormatGuid(d.group).c_str());
      break;
    }
    case nevr_lobby_frame::Kind::Join:
      Log(EchoVR::LogLevel::Info, "[NEVR.WS] lobby join requested conn=%d lobby=%s entrants=%zu accounts=%s", connIdx,
          nevr_lobby_frame::FormatGuid(d.lobby).c_str(), d.entrants.size(),
          nevr_lobby_frame::FormatAccounts(d.entrants).c_str());
      break;
    case nevr_lobby_frame::Kind::Success:
    case nevr_lobby_frame::Kind::Failure:
      // The text is lobby_frame.h's FormatSessionRecord, the Quest router's too (#1429).
      Log(EchoVR::LogLevel::Info, "%s",
          nevr_lobby_frame::FormatSessionRecord(d, connIdx, LobbyParty(partyView), request).c_str());
      break;
  }
}

// Info-level trace of the social message families (friends, party, social) crossing the bridge,
// walking EVERY message in a frame ([marker(8)][symbol(8)][length(8)][payload]...), not just the
// first. The per-message Debug lines are dropped at the default level, so without this a missing
// roster or party is invisible: nothing says whether the server sent the notifies or whether the
// client received them. Only the symbol name and payload length are logged, never the payload.
//
// Server->game friend messages also feed the facade's friend roster: the game's own friends code
// is not present (pnsrad exports no Social object), so SNSFriendListResponse and
// SNSFriendStatusNotify are the only place the friend list exists on the client.
static void ObserveSocialFrames(const char* direction, int connIdx, const std::string& frame) {
  size_t offset = 0;
  nevr_evr_codec::Message message;
  // A bad marker, a truncated message or the end of the frame all stop the walk: never read past it.
  while (nevr_evr_codec::ReadMessage(frame, offset, &message) == nevr_evr_codec::ReadStatus::Ok) {
    const uint64_t sym = message.symbol;
    const uint64_t len = message.length;
    const bool fromServer = strcmp(direction, "server->game") == 0;
    const uint8_t* payload = message.payload;
    LogClientLobbyMessage(connIdx, fromServer, sym, payload, len);
    // A display name arrives in a profile reply, whichever side asked for it (the game asks when a
    // friend is opened; the runtime asks for each friend, below).
    if (fromServer && sym == nevr_social_names::kProfileSuccess) {
      uint64_t accountId = 0;
      std::string displayName;
      const bool decoded = nevr_social_names::DecodeProfile(payload, static_cast<size_t>(len), &accountId, &displayName);
      uint64_t replyFor = accountId;
      if (!decoded && len >= 16) {
        replyFor = 0;
        for (int i = 7; i >= 0; --i) replyFor = (replyFor << 8) | payload[8 + i];
      }
      if (decoded) {
        nevr_social_roster::Global().SetName(accountId, displayName);
        nevr_social_party::Global().SetName(accountId, displayName);
        if (nevr_social_roster::RecentlyMet().SetRequestName(accountId, displayName)) {
          Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend request named account=%llu name=\"%s\"",
              static_cast<unsigned long long>(accountId), displayName.c_str());
        }
      }
      if (nevr_social_roster::Global().Contains(replyFor)) {
        if (decoded) {
          Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend name resolved account=%llu name_bytes=%zu",
              static_cast<unsigned long long>(replyFor), displayName.size());
        } else {
          Log(EchoVR::LogLevel::Info,
              "[NEVR.SOCIAL] friend profile reply could not be read account=%llu reply_bytes=%zu",
              static_cast<unsigned long long>(replyFor), static_cast<size_t>(len));
        }
      }
    }
    // Profile requests and replies, both ways, with the EvrId they carry (platform u64, account u64):
    // the game files a reply under "%s-%llu" of that id and drops one that matches no request it sent
    // (FUN_14060cdb0 / FUN_140610e70), so a mismatch is only visible here.
    {
      const char* profileName = EchoVR::LookupSymbolName(sym);
      if (profileName != nullptr && strstr(profileName, "OtherUserProfile") != nullptr && len >= 16) {
        uint64_t platform = 0;
        uint64_t account = 0;
        memcpy(&platform, payload, sizeof(platform));
        memcpy(&account, payload + 8, sizeof(account));
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] %s conn=%d %s platform=%llu account=%llu payload_bytes=%llu", direction,
            connIdx, profileName, static_cast<unsigned long long>(platform), static_cast<unsigned long long>(account),
            static_cast<unsigned long long>(len));
      }
    }
    // Party symbols come from our own verified tables: the game's symbol table has no names for the
    // party replies and mislabels the create hash, so it is the fallback, not the first choice.
    const char* gameName = EchoVR::LookupSymbolName(sym);
    const char* name = nevr_social_party::RequestName(sym);
    if (name == nullptr) name = nevr_social_party::ReplyName(sym);
    if (name == nullptr) name = gameName;
    if (name != nullptr && (strstr(name, "Friend") != nullptr || strstr(name, "Party") != nullptr ||
                            strstr(name, "Social") != nullptr)) {
      // An invite's target is the last u64 of the Standard party payload (social_party.h Standard):
      // logged so a test, or a person reading the log, can see who an invite went to.
      if ((strcmp(name, "PartyInviteRequest") == 0 || strcmp(name, "FriendInviteRequest") == 0) && len >= 0x28) {
        uint64_t target = 0;
        memcpy(&target, payload + 0x20, sizeof(target));
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] %s conn=%d %s payload_bytes=%llu target=%llu", direction,
            connIdx, name, static_cast<unsigned long long>(len), static_cast<unsigned long long>(target));
      } else if (strcmp(name, "PartyInviteResponse") == 0 && len >= 0x2C) {
        // Targeted payload (social_party.h Targeted): self UUID, target UUID, session, then the
        // param at +0x28: 1 = accept, 0 = dismiss.
        uint32_t param = 0;
        memcpy(&param, payload + 0x28, sizeof(param));
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] %s conn=%d %s payload_bytes=%llu param=%u", direction, connIdx,
            name, static_cast<unsigned long long>(len), param);
      } else {
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] %s conn=%d %s payload_bytes=%llu", direction, connIdx,
            name, static_cast<unsigned long long>(len));
      }
    }
    // Self-check "party_data_share" (#398): the service's answer to a party data share, by its symbol.
    if (fromServer) nevr_party_share_check::OnServerMessage(sym);
    else nevr_party_share_check::OnClientMessage(sym, payload, static_cast<size_t>(len));
    // Self-check "party_member_leave" (#403): a PartyLeaveNotify the game client receives.
    if (fromServer) nevr_party_leave_check::OnServerMessage(sym, payload, static_cast<size_t>(len));
    // Self-check "friend_request" (#405): an incoming friend request, a repeat of it, or its end.
    if (fromServer) nevr_friend_request_check::OnServerMessage(sym, payload, static_cast<size_t>(len));
    if (fromServer && sym == nevr_evr_codec::kSymLoginSuccess) {
      // Self-checks may send from here on, as the user the service named (payload: session UUID, platform, account).
      if (len >= nevr_evr_codec::kUuidSize + 16) {
        nevr_evr_codec::UserId user;
        user.platformCode = nevr_evr_codec::ReadLE64(payload + nevr_evr_codec::kUuidSize);
        user.accountId = nevr_evr_codec::ReadLE64(payload + nevr_evr_codec::kUuidSize + 8);
        nevr_self_check::SetLoggedIn(true, user);
      }
      // A new session (first login, reconnect or another account): the friend requests that were pending
      // belonged to the last one. The server replays this player's after the subscribe that follows.
      const size_t cleared = nevr_social_roster::RecentlyMet().ClearRequests();
      if (cleared != 0) {
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend requests cleared: new session (login success) removed=%zu",
            cleared);
      }
    }
    if (fromServer && sym == nevr_social_roster::kFriendPresenceNotify) {
      uint64_t friendId = 0;
      nevr_social_roster::Presence presence;
      if (nevr_social_roster::ParsePresenceNotify(payload, static_cast<size_t>(len), &friendId, &presence)) {
        nevr_social_roster::Global().SetPresence(friendId, presence);
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend presence account=%llu party=%llu joinable=%d text=\"%s\"",
            static_cast<unsigned long long>(friendId), static_cast<unsigned long long>(presence.partyId),
            presence.joinable ? 1 : 0, presence.text.c_str());
      } else {
        Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] friend presence could not be read bytes=%llu",
            static_cast<unsigned long long>(len));
      }
    }
    if (fromServer && sym == nevr_social_roster::kRecentlyMetListResponse) {
      // Recently met (proposal §2): the whole list; the refresh the game is polling (slot 56) ends.
      std::vector<nevr_social_roster::Entry> people;
      if (nevr_social_roster::ParseRecentlyMetResponse(payload, static_cast<size_t>(len), &people)) {
        const auto online = static_cast<unsigned>(std::count_if(people.begin(), people.end(),
                                                                [](const nevr_social_roster::Entry& e) { return e.online; }));
        const size_t count = people.size();
        nevr_social_roster::RecentlyMet().SetList(std::move(people));
        // After the store: a reader that waits for this line (the scenario harness) finds the list in place.
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] recently met list count=%zu online=%u", count, online);
      } else {
        nevr_social_roster::RecentlyMet().EndRefresh();
        Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] recently met list could not be read bytes=%llu",
            static_cast<unsigned long long>(len));
      }
    }
    if (fromServer && sym == nevr_social_party::kPartyDataNotify) {
      // Party or member data (proposal §3): only a JSON object goes on to the game's CJson loader.
      nevr_social_party::DataNotify notify;
      if (!nevr_social_party::ParseDataNotify(payload, static_cast<size_t>(len), &notify)) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] party data could not be read bytes=%llu",
            static_cast<unsigned long long>(len));
      } else if (const nlohmann::json parsed = nlohmann::json::parse(notify.json, nullptr, false);
                 parsed.is_discarded() || !parsed.is_object()) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] party data rejected: not a JSON object party=%llu member=%llu bytes=%zu",
            static_cast<unsigned long long>(notify.partyId), static_cast<unsigned long long>(notify.memberId),
            notify.json.size());
      } else {
        const nevr_social_party::DataOutcome outcome =
            nevr_social_party::Global().ReceiveData(notify.partyId, notify.memberId, notify.json);
        const auto headset = parsed.find("headsettype");
        Log(EchoVR::LogLevel::Info,
            "[NEVR.SOCIAL] party data received party=%llu member=%llu seq=%u bytes=%zu keys=%zu headsettype=%s outcome=%s",
            static_cast<unsigned long long>(notify.partyId), static_cast<unsigned long long>(notify.memberId),
            notify.seq, notify.json.size(), parsed.size(),
            headset != parsed.end() ? headset->dump().c_str() : "-", nevr_social_party::DataOutcomeName(outcome));
      }
    }
    if (fromServer) {
      if (gameName != nullptr) {
        nevr_social_roster::Feed(nevr_social_roster::Global(), gameName, payload, static_cast<size_t>(len));
        // The friends the server names get a name lookup, once each per session.
        uint64_t friendId = 0;
        uint8_t status = 0;
        if (strcmp(gameName, "FriendStatusNotify") == 0 &&
            nevr_social_roster::ParseStatusNotify(payload, static_cast<size_t>(len), &friendId, &status)) {
          const std::vector<nevr_social_party::Message> asks = nevr_social_names::GlobalResolver().Want(friendId);
          if (!asks.empty()) {
            Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend name lookup requested account=%llu sent=%d",
                static_cast<unsigned long long>(friendId), nevr_social_party::Send(asks) ? 1 : 0);
          }
        }
      }
      // A friend added, accepted, removed or withdrawn: none of these carries presence, so ask the
      // server for the list again; the reply rebuilds the roster (a friend added on the website
      // would otherwise stay invisible until the next login).
      {
        // An incoming friend request is listed first in the recently-met list (the game has no prompt of its
        // own, #405); its requester's profile is asked for like a friend's.
        const nevr_social_roster::RequestEvent request = nevr_social_roster::ApplyFriendMessage(
            nevr_social_roster::RecentlyMet(), sym, payload, static_cast<size_t>(len));
        if (request.change == nevr_social_roster::RequestChange::kAdded) {
          Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend request received account=%llu pending=%zu",
              static_cast<unsigned long long>(request.account), request.pending);
          const std::vector<nevr_social_party::Message> asks = nevr_social_names::GlobalResolver().Want(request.account);
          if (!asks.empty()) {
            Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend name lookup requested account=%llu sent=%d",
                static_cast<unsigned long long>(request.account), nevr_social_party::Send(asks) ? 1 : 0);
          }
        } else if (request.change == nevr_social_roster::RequestChange::kCleared) {
          Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend request cleared account=%llu reason=%s pending=%zu",
              static_cast<unsigned long long>(request.account), request.reason, request.pending);
        }
      }
      if (nevr_social_roster::IsFriendChangeSymbol(sym) || nevr_social_roster::IsFriendChange(gameName)) {
        uint64_t friendId = 0;
        if (len >= 16) memcpy(&friendId, payload + 8, sizeof(friendId));
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend change %s account=%llu: refreshing the friend list sent=%d",
            name != nullptr ? name : "<unnamed>", static_cast<unsigned long long>(friendId),
            nevr_social_party::Send(nevr_social_party::Global().RefreshFriends()) ? 1 : 0);
      }
      // A removed friend leaves the roster now, not when the refresh above lands (social_roster.h).
      {
        uint64_t removedId = 0;
        if (nevr_social_roster::ParseFriendRemoval(sym, payload, static_cast<size_t>(len), &removedId)) {
          const bool dropped = nevr_social_roster::Global().Remove(removedId);
          Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend removed account=%llu dropped=%d",
              static_cast<unsigned long long>(removedId), dropped ? 1 : 0);
        }
      }
      // Party messages update the facade's party; any requests that were waiting on the reply
      // (invites queued behind the party's creation) go out now. This runs on the remote's
      // callback thread outside g_pairsMutex, and SendFrameToServer takes it itself.
      std::vector<nevr_social_party::Message> outgoing;
      const uint64_t nowSeconds = static_cast<uint64_t>(std::time(nullptr));
      if (nevr_social_party::Global().Feed(sym, payload, static_cast<size_t>(len), nowSeconds, &outgoing) &&
          !outgoing.empty()) {
        nevr_social_party::Send(outgoing);
      }
      // A party member or an invite's sender the bridge has no name for: ask for the profile, as for
      // friends, so the game shows a name instead of an account id.
      for (const uint64_t accountId : nevr_social_party::Global().TakeUnnamed()) {
        const std::vector<nevr_social_party::Message> asks = nevr_social_names::GlobalResolver().Want(accountId);
        if (!asks.empty()) {
          Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party name lookup requested account=%llu sent=%d",
              static_cast<unsigned long long>(accountId), nevr_social_party::Send(asks) ? 1 : 0);
        }
      }
    }
    offset += nevr_evr_codec::kHeaderSize + static_cast<size_t>(len);
  }
}

// Sends a frame from the runtime itself (the social facade's party requests) to the server on the
// game's login connection, as if the game had. Queues it if the remote is still connecting, as the
// game-side path does. Takes g_pairsMutex, so it must not be called with it held.
// Defined with RouterPath: when the router owns the login session (mode all) a runtime frame goes on its session
// remote (Router::SendToSession), and an injected server frame is delivered through Router::OnRemoteFrame.
static bool RouterOwnsLogin();
static bool RouterSendToSession(const std::string& frame);
[[maybe_unused]] static bool RouterInjectServerFrame(const std::string& frame);

static bool SendFrameToServer(const std::string& frame) {
  if (t_remoteSessionEnding) return false;  // see t_remoteSessionEnding: the remote this would use is closing
  bool sent = false;
  if (RouterOwnsLogin()) {
    sent = RouterSendToSession(frame);
  } else {
    // The send runs outside g_pairsMutex: a send that fails runs the remote's Close callback on this thread, and
    // that callback takes the mutex (#738).
    std::shared_ptr<ix::WebSocket> remote;
    {
      std::lock_guard<std::mutex> lock(g_pairsMutex);
      for (auto& entry : g_pairs) {
        ProxyPair& pair = *entry.second;
        if (pair.connIdx != 1 || !pair.remoteWs) continue;
        if (!pair.remoteOpen) {
          pair.pendingToRemote.push_back(frame);
          sent = true;
        } else {
          remote = pair.remoteWs;
        }
        break;
      }
    }
    if (remote) sent = remote->sendBinary(frame).success;
  }
  if (sent) {
    ObserveSocialFrames("game->server", 1, frame);
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] game->server frame not sent: no login connection");
  }
  return sent;
}

#ifdef NEVR_SCENARIO_CONTROL
// Scenario-control builds only (docs/design/2026-10-01-social-scenario-harness.md). Delivers a frame
// to the game as if the server had sent it on the login connection: the same roster/party feed,
// frame log and send to the game's socket a real server->game frame goes through.
bool InjectServerFrameForTest(const std::string& frame, std::string* error) {
  if (RouterOwnsLogin()) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] injecting a server->game frame bytes=%zu", frame.size());
    if (!RouterInjectServerFrame(frame)) {
      if (error != nullptr) *error = "no game login connection to inject into";
      return false;
    }
    return true;
  }
  ix::WebSocket* gameWs = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_pairsMutex);
    SharedRouteLocked(&gameWs);
  }
  if (gameWs == nullptr) {
    if (error != nullptr) *error = "no game login connection to inject into";
    return false;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] injecting a server->game frame bytes=%zu", frame.size());
  ObserveSocialFrames("server->game", 1, frame);
  LogFrameMessages("server->game", 1, frame);
  if (!gameWs->sendBinary(frame).success) {
    if (error != nullptr) *error = "send to the game socket failed";
    return false;
  }
  return true;
}
#endif

// The sender of the runtime's own social requests (party, friend list, profile name lookups). A dedicated server is
// not a player and sends none (#1280); each caller's own line reports the refusal as sent=0. Self-checks keep
// SendFrameToServer: their RemoteLogSet is not social.
static bool SendSocialFrameToServer(const std::string& frame) {
  if (g_isServer) return false;
  return SendFrameToServer(frame);
}

// Registers SendSocialFrameToServer as the party requests' sender when the bridge is loaded.
static const bool g_partySenderRegistered = (nevr_social_party::SetSender(&SendSocialFrameToServer), true);

// Self-checks (compat/self_check.h): a result goes out on the login connection through SendFrameToServer once
// LoginSuccess has been seen, and into the nevr log at the moment it happens. Wired from InstallWebSocketBridge
// (the game's boot), not from a static initialiser: the unit allocates.
static void LogSelfCheck(const nevr_self_check::LogRecord& record) {
  Log(EchoVR::LogLevel::Info, "[NEVR.SELFCHECK] check=%s pass=%d expected=\"%s\" observed=\"%s\"", record.name.c_str(),
      record.pass ? 1 : 0, record.expected.c_str(), record.observed.c_str());
}

static void WireSelfChecks() {
  nevr_self_check::SetEnabled(true);  // on in every build, first and unconditional (#451)
  nevr_status_redirect_check::Register(&nevr_status_redirect_check::ProcessProbe);  // #408
  nevr_credential_cache_check::Register();  // #562: credential_cache and credential_cache_refused
  nevr_self_check::SetSender(&SendFrameToServer);
  nevr_self_check::SetLogSink(&LogSelfCheck);
#ifdef NEVR_PROJECT_VERSION
  nevr_self_check::SetBuild(NEVR_PROJECT_VERSION);
#endif
}

// Platform codes, the bridge's login platform, the remote Bearer choice and the /ws path test live in
// EvrCodec (compat/evr_codec.h), shared with the Quest target.

// The HMD serial field the stock client sends (#83): relay the game's serial buffer, or its stock "N/A"
// value in No-VR mode. Outside the game (unit tests) there is nothing to read.
#ifdef NEVR_TEST_HOOKS
static std::atomic<int64_t> g_testGameFlagWord{-1};
#endif

// Test seam: runs just before a shared-route frame is sent to its target, so a test can hold the send open while
// the target's last owner lets go. Production builds compile the call to nothing.
#ifdef NEVR_TEST_HOOKS
static std::atomic<void (*)(ix::WebSocket*)> g_testBeforeSharedSend{nullptr};
#define NEVR_BEFORE_SHARED_SEND(t) do { if (auto h_ = g_testBeforeSharedSend.load()) h_(t); } while (0)
#else
#define NEVR_BEFORE_SHARED_SEND(t) ((void)0)
#endif

static std::optional<uint32_t> GameFlagWord() {
  if (EchoVR::g_GameBaseAddress == nullptr || g_pGame == nullptr) return std::nullopt;
  uint32_t flags = 0;
  memcpy(&flags, static_cast<const char*>(g_pGame) + 0x7AE0, sizeof(flags));
  return flags;
}

static nevr_hmd_serial::Choice GameHmdSerial() {
  const std::optional<uint32_t> flags = GameFlagWord();
  if (!flags) return nevr_hmd_serial::Select(false, nullptr);
  const char* serial = reinterpret_cast<const char*>(EchoVR::g_GameBaseAddress) + 0x20C7834;
  return nevr_hmd_serial::Select((*flags & nevr_hmd_serial::kNoVrFlag) != 0, serial);
}

// The flag word the login decision reads. A test can stand in for the game's word; the HMD serial still reads the
// game's own memory, which a test run does not have.
static std::optional<uint32_t> LoginFlagWord() {
#ifdef NEVR_TEST_HOOKS
  const int64_t forced = g_testGameFlagWord.load();
  if (forced >= 0) return static_cast<uint32_t>(forced);
#endif
  return GameFlagWord();
}

// The No VR decision, read where the HMD serial reads it (#1082): the game's own flag at login time.
static bool GameIsNoVr() {
  const std::optional<uint32_t> flags = LoginFlagWord();
  return flags && nevr_hmd_serial::LoginIsNoVr(g_isServer != FALSE, *flags);
}

// What the login chose and from which flag word, for the run's log: the xpid in `login injected` shows the result,
// this shows the input. Only logged where a login is injected.
static void LogLoginPlatform(uint64_t platformCode) {
  const std::optional<uint32_t> flags = LoginFlagWord();
  Log(EchoVR::LogLevel::Info, "[NEVR.WS] login platform=%d (%s) no_vr=%d game_flags=0x%x is_server=%d",
      static_cast<int>(platformCode), nevr_evr_codec::PlatformPrefix(platformCode),
      GameIsNoVr() ? 1 : 0, flags ? *flags : 0u, g_isServer != FALSE ? 1 : 0);
}

static std::string BuildLoginRequest(uint64_t discordId, uint64_t platformCode = 2,
                                     const std::string& displayName = std::string(),
                                     const std::string& accessToken = std::string(),
                                     const std::string& password = std::string()) {
  // Platform codes: see nevr_evr_codec::PlatformPrefix (1-indexed: STM=1 ... OVR_ORG=4 ... DMO=7).
  uint64_t accountId = discordId;

  // Host facts, MEASURED. No value in this block is a literal ("cpu":"Wine",
  // "video_card":"Wine D3D12", 4 physical cores, 8 logical, 16384 MB total,
  // 8192 used would be sent as though read from the machine). That is
  // worse than sending nothing: absent data is visibly absent, while invented
  // data is indistinguishable from a reading and gets acted on.
  //
  // Fields this process cannot honestly determine are sent EMPTY or 0
  // rather than guessed. video_card and dedicated_gpu_memory have no truthful
  // answer on a headless server with no device enumerated, and network_type
  // was never anything but a guess. Empty is a true statement; "Wine D3D12" is
  // not. N112.
  const nevr_system_info::Host& host = nevr_system_info::Get();
  const std::string driverVersion =
      host.IsWine() ? ("Wine " + host.wine_version +
                       (host.wine_host_os.empty() ? "" : " on " + host.wine_host_os))
                    : std::string();

  // LoginProfile JSON — matches the game's SNSLogInRequestv2 format.
  // Keep its construction portable so Quest and Windows use the same fields
  // and JSON escaping rules.
  const nevr_build_identity::Info& buildId = nevr_build_identity::Get();
  const std::string pluginManifest = BuildPluginManifestJson();
  // The serial is the one the stock client sends: the game's serial buffer in
  // VR, "N/A" with no VR, "unknown" only when the game has none. Only its source
  // and length are logged, never the value.
  const nevr_hmd_serial::Choice hmd = GameHmdSerial();
  Log(EchoVR::LogLevel::Info, "[NEVR.WS] login hmd serial source=%s length=%zu",
      nevr_hmd_serial::SourceName(hmd.source), hmd.value.size());

  // nevr_plugins lists every configured plugin with what the loader did with it
  // (loaded, failed, or disabled). The manifest is parsed so the login field is a
  // JSON array, not a string-escaped copy of one. The non-throwing parse does not
  // fail on the builder's own nlohmann output; if it ever did, the login still
  // goes out with an empty list and a Warning that says so.
  nlohmann::json plugins = nlohmann::json::parse(pluginManifest, nullptr, false);
  if (plugins.is_discarded() || !plugins.is_array()) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.WS] login nevr_plugins: plugin report is not a JSON array (%zu bytes) — sending []",
        pluginManifest.size());
    plugins = nlohmann::json::array();
  }
  size_t loaded = 0;
  for (const nlohmann::json& plugin : plugins) {
    if (plugin.is_object() && plugin.value("loaded", false)) ++loaded;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.WS] login nevr_plugins configured=%zu loaded=%zu", plugins.size(),
      loaded);

  nevr_login_profile::LoginProfileInputs profileInputs;
  profileInputs.account_id = accountId;
  profileInputs.display_name = displayName;
  profileInputs.access_token = accessToken;
  profileInputs.password = password;
  profileInputs.hmd_serial_number = hmd.value;
  profileInputs.driver_version = driverVersion;
  profileInputs.cpu = host.cpu_brand;
  profileInputs.physical_cores = host.physical_cores;
  profileInputs.logical_cores = host.logical_cores;
  profileInputs.memory_total_mb = host.memory_total_mb;
  profileInputs.memory_used_mb = host.memory_used_mb;
  profileInputs.project_version = buildId.project_version;
  profileInputs.git_commit = buildId.git_commit;
  profileInputs.git_describe = buildId.git_describe;
  profileInputs.build_type = buildId.build_type;
  profileInputs.social_level = nevr_social_party::DeclaredSocialLevel(g_isServer != FALSE);
  profileInputs.plugins = std::move(plugins);
  // Assets the loaded plugins registered (#432). The report is built by the registry's own nlohmann output, so
  // the non-throwing parse only guards a future change to it.
  nlohmann::json assets = nlohmann::json::parse(nevr::assets::Global().ReportJson(), nullptr, false);
  if (assets.is_discarded() || !assets.is_array()) assets = nlohmann::json::array();
  Log(EchoVR::LogLevel::Info, "[NEVR.WS] login nevr_assets count=%zu", assets.size());
  profileInputs.assets = std::move(assets);
  const std::string jsonStr = nevr_login_profile::BuildLoginProfileJson(profileInputs);

  // Frame layout (UUID, platform, account, JSON, NUL) is EvrCodec's, shared with the Quest target.
  std::optional<std::string> frame = nevr_evr_codec::BuildLoginRequest(platformCode, accountId, jsonStr);
  if (!frame.has_value()) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.WS] login request not built: the profile JSON contains a NUL byte (%zu bytes); no login is sent",
        jsonStr.size());
    return std::string();
  }
  return std::move(*frame);
}

// ============================================================================
// Admitting a game socket (#520)
// ============================================================================

// The request target of a socket as a log field: the path only (a query can carry a credential, logging.md
// Rule 4), at most 64 characters, anything not printable shown as '?'.
static std::string LoggableTarget(const std::string& target) {
  const std::string_view path = nevr_socket_role::PathOf(target);
  std::string shown;
  for (const char c : path.substr(0, 64)) shown.push_back(c >= 0x20 && c < 0x7f ? c : '?');
  return shown.empty() ? std::string("-") : shown;
}

struct AdmittedSocket {
  nevr_socket_role::Admission admission;
  // The login session at the moment of admission, taken in the same critical section: a socket that
  // shares the session uses this and never reads g_loginRemoteWs outside g_pairsMutex.
  std::shared_ptr<ix::WebSocket> loginRemote;
};

// Takes the slot for a game socket that just opened: reads the role from its request target (`target` is
// WebSocketOpenInfo::uri), the listener that accepted it and the slots already taken, and logs the
// decision. The Open handler calls this first, before it builds the socket's pair.
static AdmittedSocket AdmitGameSocket(const ix::WebSocket* gameWs, const std::string& target,
                                      nevr_socket_role::Listener listener) {
  AdmittedSocket admitted;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    admitted.admission = g_roles.Admit(listener, nevr_socket_role::RoleFromRequestTarget(target));
    admitted.loginRemote = g_loginRemoteWs;
  }
  return admitted;
}

// The admission decision as one structured line: the slot, the role, why, the path the game asked for, which
// routing path took the socket (#708: `router` when nevr_session_router owns it, `legacy` otherwise) and the
// router's id for it (0 on the legacy path).
static void LogAdmission(const AdmittedSocket& admitted, const ix::WebSocket* gameWs, const std::string& target,
                         nevr_socket_role::Listener listener, const char* path, uint64_t game) {
  Log(EchoVR::LogLevel::Info,
      "[NEVR.WS] game socket admitted conn=%d role=%s basis=%s listener=%s target=%s has_query=%s path=%s game=%llu",
      admitted.admission.connIdx, nevr_socket_role::RoleName(admitted.admission.role),
      nevr_socket_role::BasisName(admitted.admission.basis), nevr_socket_role::ListenerName(listener),
      LoggableTarget(target).c_str(), target.find('?') != std::string::npos ? "true" : "false", path,
      static_cast<unsigned long long>(game));
  Log(EchoVR::LogLevel::Debug, "[NEVR.WS] game socket conn=%d ws=%p", admitted.admission.connIdx,
      static_cast<const void*>(gameWs));
}

// The remote ws for one game socket: URL (with the percent-encoded credentials), the Bearer header and the
// identity the login is built from. Shared by the legacy path (one remote per socket) and the router path
// (RemoteTransport::Open), so both put the same bytes on the wire. `connIdx` is only for log lines.
struct PreparedRemote {
  std::shared_ptr<ix::WebSocket> remote;  // null: nothing to connect (a refused login, #1254); the caller ends the game socket
  std::string refusalKey;                 // the sign-in the remote presents, for ClientRefusalLatch
  std::string bearerToken;
  std::string accountName;  // N123 - empty means "not known", never a placeholder
  uint64_t discordId = 0;
  // #1446: the sign-in ended without a token, so the caller answers the game's login itself (AnswerLoginLocally) and
  // keeps the game socket open. No remote is made.
  bool answerLocally = false;
};

// What the login is built from: the token-auth token, the account id (JWT, then the config.yaml fallback, N20) and
// the display name (N123). `verbose` false reads without the log lines (the router's login builder runs this
// after PrepareRemote has logged them for the same session).
struct LoginIdentity {
  std::string bearerToken;
  std::string accountName;  // N123 - empty means "not known", never a placeholder
  uint64_t discordId = 0;
};

static LoginIdentity ReadLoginIdentity(bool verbose) {
  LoginIdentity identity;
  std::string& bearerToken = identity.bearerToken;
  std::string& accountName = identity.accountName;
  uint64_t& discordId = identity.discordId;
  {
    auto getTokenFn = reinterpret_cast<const char* (*)()>(ResolveModuleProc("TokenAuth_GetToken"));
    auto getDiscordIdFn = reinterpret_cast<uint64_t (*)()>(ResolveModuleProc("TokenAuth_GetDiscordId"));
    // N123. Optional by design: an older token_auth.dll without this
    // export must still load. A null here means "no name available",
    // which BuildLoginRequest already handles.
    auto getUsernameFn = reinterpret_cast<const char* (*)()>(ResolveModuleProc("TokenAuth_GetUsername"));
    if (getTokenFn) {
      const char* tok = getTokenFn();
      if (tok) bearerToken = tok;
    }
    if (getDiscordIdFn) discordId = getDiscordIdFn();
    if (getUsernameFn) {
      const char* name = getUsernameFn();
      if (name) accountName = name;
    }
  }
  // N92 + N20: config fallback, ported from the ws-bridge module during the
  // monolithic fold. Without it the fold logs in with account id 0 —
  // measured: "login injected xpid=DSC-NOVR-0". The module had this and
  // this copy did not, which is the divergence N92 is about.
  //
  // N20 (owner decision, 2026-07-27): the fallback applies in CLIENT mode
  // too, not only when g_isServer. JWT first, config second, regardless of
  // mode.
  if (discordId == 0) {
    // N133 S4a: fallback discord id from config.yaml identity.discord_id
    // (was the game JSON nevr_discord_id). NOT gated on g_isServer — N20
    // (owner, 2026-07-27) applies the fallback in client mode too.
    const char* cfgId = NevrCfgGetFlat("nevr_discord_id");
    if (cfgId && cfgId[0] != '\0') {
      discordId = strtoull(cfgId, nullptr, 10);
      if (verbose) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.WS] Using nevr_discord_id from config: %llu",
            static_cast<unsigned long long>(discordId));
      }
    }
  }
  // #1446: said once per process. A game that reconnects its login socket reads this on every attempt, and the same
  // line each time says nothing new.
  static std::atomic<bool> s_noDiscordIdSaid{false};
  if (discordId == 0 && verbose && !s_noDiscordIdSaid.exchange(true)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.WS] No discord ID from JWT or config — LoginRequest will use "
        "account ID 0");
  }
  return identity;
}

// Sets the CNSUser's login state to "logging in" so CNSUser::LogInSuccessCB processes the server's LoginSuccess
// (it discards it while the state at +0x90 is 0). Only the actual login connection may do this: a later
// connection resetting it loses the game's logged-in status during a lobby join.
static void PatchCnsUserLoginState(uint64_t discordId, uint64_t platformCode) {
  HMODULE hPnsrad = GetModuleHandleA("pnsrad.dll");
  if (hPnsrad) {
    typedef void* (*UsersFn)();
    auto Users = reinterpret_cast<UsersFn>(GetProcAddress(hPnsrad, "Users"));
    if (Users) {
      auto* usersObj = reinterpret_cast<uint8_t*>(Users());
      if (usersObj) {
        uint64_t userCount = *reinterpret_cast<uint64_t*>(usersObj + 0x398);
        uint8_t** bufCtx = *reinterpret_cast<uint8_t***>(usersObj + 0x368);
        if (userCount > 0 && bufCtx && *bufCtx) {
          uint8_t* user = *bufCtx;
          int64_t*  accountId  = reinterpret_cast<int64_t*>(user + 0x88);
          uint64_t* loginState = reinterpret_cast<uint64_t*>(user + 0x90);
          uint32_t* stateFlags = reinterpret_cast<uint32_t*>(user + 0x9c);
          int64_t  beforeAcct    = *accountId;
          uint64_t beforeState   = *loginState;
          uint32_t beforeFlags   = *stateFlags;
          int      beforeProvider = static_cast<int>(beforeState & 0xf);
          // Set the user's XPID: account_id and provider enum.
          // +0x88 = account_id (discord ID from JWT)
          // +0x90 low nibble = provider enum, the platform this login carries
          //   (4 OVR-ORG, or 7 DMO for a No VR client; 2 = PSN in binary,
          //   patched to DSC by PatchDscProvider string table rewrite)
          // +0x9c = state flags (0x04 = connected/logged in)
          *accountId  = static_cast<int64_t>(discordId);
          *loginState = nevr_provider_identity::WithLoginProvider(*loginState, platformCode);  // game numbering
          *stateFlags = 0x04;
          Log(EchoVR::LogLevel::Info,
              "[NEVR.WS] CNSUser login state patched acct=%lld->%lld "
              "state=0x%llx->0x%llx provider=%d->%d flags=0x%x->0x%x "
              "(unblocks LogInSuccessCB)",
              static_cast<long long>(beforeAcct), static_cast<long long>(*accountId),
              static_cast<unsigned long long>(beforeState), static_cast<unsigned long long>(*loginState),
              beforeProvider, static_cast<int>(*loginState & 0xf), beforeFlags, *stateFlags);
        }
      }
    }
  }
}

// Forwards a frame the game sent on `gameWs` to the remote its pair is bound to (queued while that remote is not
// open yet).
static void ForwardGameFrameToRemote(ix::WebSocket& gameWs, const std::string& frame, bool binary) {
  // The send runs outside g_pairsMutex: a send that fails runs the remote's Close callback on this thread, and
  // that callback takes the mutex (#738).
  std::shared_ptr<ix::WebSocket> remote;
  int connIdx = -1;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    auto it = g_pairs.find(&gameWs);
    if (it == g_pairs.end()) {
      // N.B.: if g_pairs loses an entry (e.g. a Close/Message race) while the
      // game keeps sending on that socket, every subsequent message would
      // independently re-trigger this WARNING with zero new signal after the
      // first — count instead of flooding.
      static std::atomic<uint64_t> s_droppedCount{0};
      uint64_t dropped = ++s_droppedCount;
      if (dropped == 1 || dropped % 100 == 0) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.WS]   -> DROPPED (no pair found) — %llu total occurrences",
            static_cast<unsigned long long>(dropped));
      }
      return;
    }
    auto& pair = it->second;
    CheckFirstFrameRoleLocked(*pair, frame);
    if (!pair->remoteOpen) {
      pair->pendingToRemote.push_back(frame);
      Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   -> queued (remote not open yet, %zu pending)",
          pair->pendingToRemote.size());
      return;
    }
    // A pair that shares its remote with a pair still flushing queues behind that flush, on the flushing pair's
    // queue, so frames from both sockets reach the remote in the order they arrived here.
    for (auto& entry : g_pairs) {
      ProxyPair& other = *entry.second;
      if (&other != pair.get() && other.remoteWs == pair->remoteWs && !other.remoteOpen) {
        other.pendingToRemote.push_back(frame);
        Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   -> queued behind the login flush (%zu pending)",
            other.pendingToRemote.size());
        return;
      }
    }
    remote = pair->remoteWs;
    connIdx = pair->connIdx;
  }
  // A shared login session can die while the game sits idle; the game then
  // waits forever on its MATCHMAKING screen. Say so instead of dropping quietly.
  const ix::ReadyState remoteState = remote->getReadyState();
  bool sent = false;
  if (binary) {
    sent = remote->sendBinary(frame).success;
    Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   -> forwarded (success=%s)", sent ? "true" : "false");
  } else {
    sent = remote->sendText(frame).success;
  }
  if (!sent || remoteState != ix::ReadyState::Open) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.WS] game->server message NOT delivered: conn=%d (%s) send_success=%s "
        "remote_state=%s bytes=%zu — the remote session is gone; the game will wait "
        "on this request indefinitely",
        connIdx, ConnLabel(connIdx), sent ? "true" : "false", RemoteStateName(remoteState), frame.size());
  }
}

// True while `pair` is still owned by g_pairs (a game socket that closed meanwhile takes its pair with it).
static bool PairRegisteredLocked(const ProxyPair* pair) {
  for (const auto& entry : g_pairs) {
    if (entry.second.get() == pair) return true;
  }
  return false;
}

// The remote's Open: the login request goes out on the login socket, and what the game sent meanwhile is flushed,
// in order, before the pair takes frames directly (remoteOpen stays false until then, so a frame that arrives
// during the flush queues behind it). Every send runs outside g_pairsMutex: a send that fails runs the remote's
// Close callback on this thread, and that callback takes the mutex (#738).
static void HandleRemoteOpen(ProxyPair* pair, int connIdx, uint64_t discordId, const std::string& accountName,
                             const std::string& bearerToken) {
  std::shared_ptr<ix::WebSocket> remote;
  std::vector<std::string> batch;
  uint64_t injectedPlatform = 0;
  size_t injectedSize = 0;
  bool injected = false;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    if (!PairRegisteredLocked(pair)) return;
    remote = pair->remoteWs;
    const std::string diagnostic = nevr_log_diagnostics::FormatRedactedUrlDiagnostic(
        "[NEVR.WS] Remote open (conn=" + std::to_string(connIdx) + ", " + ConnLabel(connIdx) + "): ",
        g_remoteUri);
    Log(EchoVR::LogLevel::Debug, "%s", diagnostic.c_str());

    // Inject LoginRequest on the login socket (not config, and not a further /login: N91).
    // pnsrad.dll won't send its own because it has no user identity
    // (OVR SDK is bypassed). The LoginRequest is built and injected here.
    //
    // Before injecting, set the CNSUser's login state to "logging in"
    // so that CNSUser::LogInSuccessCB processes the server's LoginSuccess
    // response. Without this, LogInSuccessCB silently discards the message
    // because the user's login state at +0x90 is still 0 (logged out).
    if (pair->role == nevr_socket_role::Role::Login && !pair->loginInjected) {
      pair->loginInjected = true;

      // Set CNSUser login state only on the actual login connection.
      // Later connections (matchmaker, etc.) must not reset the state
      // or the game loses its logged-in status during lobby join.
      std::string cfgPasswordStr;
      {
        const char* cfgDiscordId = NevrCfgGetFlat("nevr_discord_id");
        const char* cfgPassword = NevrCfgGetFlat("nevr_password");
        bool hasUrlCreds = cfgDiscordId && cfgDiscordId[0] && cfgPassword && cfgPassword[0];
        injectedPlatform = nevr_evr_codec::SelectPlatformCode(hasUrlCreds, GameIsNoVr());
        if (cfgPassword) cfgPasswordStr = cfgPassword;
      }
      LogLoginPlatform(injectedPlatform);
      PatchCnsUserLoginState(discordId, injectedPlatform);
      g_lastInjectedDiscordId = discordId;
      nevr_social_party::Global().SetSelf(discordId, accountName, injectedPlatform);
      std::string loginMsg = BuildLoginRequest(discordId, injectedPlatform, accountName, bearerToken, cfgPasswordStr);
      if (loginMsg.empty()) {
        Log(EchoVR::LogLevel::Error, "[NEVR.WS] login NOT injected conn=%d (%s): the request could not be built",
            connIdx, ConnLabel(connIdx));
      } else {
        injected = true;
        injectedSize = loginMsg.size();
        batch.push_back(std::move(loginMsg));
      }
    }
    batch.insert(batch.end(), std::make_move_iterator(pair->pendingToRemote.begin()),
                 std::make_move_iterator(pair->pendingToRemote.end()));
    pair->pendingToRemote.clear();
  }

  bool loginLogged = !injected;
  for (;;) {
    for (const std::string& frame : batch) remote->sendBinary(frame);
    if (!loginLogged) {
      loginLogged = true;
      const std::string xpid =
          std::string(nevr_evr_codec::PlatformPrefix(injectedPlatform)) + "-" + std::to_string(discordId);
      Log(EchoVR::LogLevel::Info, "[NEVR.WS] login injected xpid=%s platform=%d conn=%d (%s) size=%zu", xpid.c_str(),
          static_cast<int>(injectedPlatform), connIdx, ConnLabel(connIdx), injectedSize);
    }
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    // The address alone can be handed to a new pair once the old one is freed; the remote this call holds
    // cannot be reused while it holds it, so a pair that now has another remote is not the one it opened for.
    if (!PairRegisteredLocked(pair) || pair->remoteWs != remote) return;
    if (pair->pendingToRemote.empty()) {
      pair->remoteOpen = true;
      return;
    }
    batch.clear();
    batch.swap(pair->pendingToRemote);
  }
}

// N149 (#1254): a refused login. A client cannot stop, and the game reconnects a closed login socket by itself, so the
// refusal is said once per refused sign-in and the later sockets of that sign-in are ended without a trip to the
// service. Own mutex: the Error callback runs on the remote's thread and PrepareRemote on the game socket's.
static std::mutex g_refusalMutex;
static nevr_login_refusal::ClientRefusalLatch& g_refusalLatch = *new nevr_login_refusal::ClientRefusalLatch();

// #1446: the logins AnswerLoginLocally answers, counted per window. Guarded by g_refusalMutex.
static nevr_login_refusal::LocalAnswerCounter& g_localAnswers = *new nevr_login_refusal::LocalAnswerCounter();

// The sign-in each router remote presents, by connection index, for the refused hook.
static std::map<nevr_session_router::RemoteId, std::string> g_routerRefusalKeys;  // by remote: connIdx is reused across sessions

static bool ClientRefusedAlready(const std::string& key) {
  std::lock_guard<std::mutex> lk(g_refusalMutex);
  return g_refusalLatch.IsRefused(key);
}

// #1446: answers a game login whose sign-in ended without a token. The game gets a LoginFailure, the socket stays open
// so the game does not reconnect in a loop, and the login slot it took is given back, so the next /login is a login
// again. The answers are counted per window (LocalAnswerCounter): a game that keeps reconnecting writes one line per
// window, not one per attempt. The frame is the one the service would send for a refused login (legacy_codec.cpp).
static void AnswerLoginLocally(ix::WebSocket* gameWs, int connIdx) {
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    g_roles.LoginSessionEnded();
  }
  const std::optional<std::string> frame = nevr_evr_codec::BuildLoginFailure(
      nevr_login_refusal::kNoSignInLoginFailureStatus, nevr_login_refusal::kNoSignInLoginFailureText);
  if (!frame) {
    Log(EchoVR::LogLevel::Error, "[NEVR.WS] conn=%d (%s): could not build the LoginFailure answer", connIdx,
        ConnLabel(connIdx));
    return;
  }
  if (!gameWs->sendBinary(*frame).success) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.WS] conn=%d (%s): the LoginFailure answer was not sent", connIdx,
        ConnLabel(connIdx));
  }
  const int64_t nowMs = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
  nevr_login_refusal::LocalAnswerCounter::Verdict verdict;
  {
    std::lock_guard<std::mutex> lk(g_refusalMutex);
    verdict = g_localAnswers.Record(nowMs);
  }
  if (!verdict.write) return;
  if (verdict.first) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.WS] login answered locally conn=%d (%s) status=%llu: the sign-in ended without a token, so the service "
        "was not asked; the game got a LoginFailure and its socket stays open",
        connIdx, ConnLabel(connIdx),
        static_cast<unsigned long long>(nevr_login_refusal::kNoSignInLoginFailureStatus));
    return;
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.WS] login attempts answered locally count=%llu window_ms=%lld: the same answer, no service trip",
      static_cast<unsigned long long>(verdict.count),
      static_cast<long long>(nevr_login_refusal::LocalAnswerCounter::kWindowMs));
}

// The remote's upgrade came back with `httpStatus`. A server stops with the cause (ServerFatal exits in server mode:
// without the account the community service accepts there is no ServerDB registration and no HTTP listener); a client
// says it once for this sign-in. A status that is not a refusal (an outage, a missing route) is left to the callers.
static void OnRemoteLoginRefused(int httpStatus, const std::string& refusalKey, int connIdx) {
  if (!nevr_login_refusal::IsLoginRefusalStatus(httpStatus)) return;
  if (g_isServer) {
    const char* cfgPassword = NevrCfgGetFlat("nevr_password");
    const bool deviceSignIn =
        nevr_login_refusal::ServerSignsInByDeviceCode(cfgPassword ? std::string_view(cfgPassword) : std::string_view());
    const std::string cause = nevr_login_refusal::RefusedLoginCause(httpStatus, NevrCfgSource(), deviceSignIn);
    ServerFatal("%s", cause.c_str());
    return;
  }
  bool firstTime = false;
  {
    std::lock_guard<std::mutex> lk(g_refusalMutex);
    firstTime = g_refusalLatch.ShouldSay(refusalKey);
  }
  if (firstTime) {
    // #1293: one line per refused sign-in, with the cause, the config loaded and the next step (ClientRefusedCause).
    const std::string cause = nevr_login_refusal::ClientRefusedCause(httpStatus, NevrCfgSource());
    Log(EchoVR::LogLevel::Warning, "[NEVR.WS] login refused conn=%d (%s): %s", connIdx, ConnLabel(connIdx),
        cause.c_str());
  }
}

// #1446: whether token-auth's device sign-in has ended without a token (TokenAuth_SignInEndedWithoutToken). Read on each
// login attempt. A sign-in still running answers false, so the login goes to the service as it did before.
static bool SignInEndedWithoutTokenNow() {
  auto signInEndedWithoutToken = reinterpret_cast<bool (*)()>(ResolveModuleProc("TokenAuth_SignInEndedWithoutToken"));
  return signInEndedWithoutToken != nullptr && signInEndedWithoutToken();
}

static PreparedRemote PrepareRemote(nevr_socket_role::Role role, int connIdx) {
  // N149 (#1254): a server's credential for the community service is either the discord id and password in
  // config.yaml or, with no password (#1532), the token its device sign-in got. Without one the login would go out
  // as account 0, or unauthenticated, and be refused every few seconds for ever, while nothing else of the server
  // (ServerDB, the HTTP listener) can start. Stop with the cause instead.
  if (g_isServer) {
    const char* cfgDiscordId = NevrCfgGetFlat("nevr_discord_id");
    const char* cfgPassword = NevrCfgGetFlat("nevr_password");
    const std::string_view password = cfgPassword ? std::string_view(cfgPassword) : std::string_view();
    const std::vector<std::string> missing = nevr_login_refusal::MissingServerCredentialKeys(
        cfgDiscordId ? std::string_view(cfgDiscordId) : std::string_view(), password);
    if (!missing.empty()) {
      const std::string cause = nevr_login_refusal::NoCredentialCause(missing, NevrCfgSource());
      ServerFatal("%s", cause.c_str());
      return PreparedRemote{};
    }
    if (nevr_login_refusal::ServerSignsInByDeviceCode(password) && ReadLoginIdentity(false).bearerToken.empty()) {
      const std::string cause = nevr_login_refusal::NoSignInCause(NevrCfgSource());
      ServerFatal("%s", cause.c_str());
      return PreparedRemote{};
    }
  }
  auto remote = std::make_shared<ix::WebSocket>();

  // Build remote URL with optional query param auth
  // (workaround: production nginx strips Bearer JWT and forces format=evr)
  std::string remoteUrl = g_remoteUri;
  // N133 S4a: discord id + password come from config.yaml
  // (identity.discord_id / auth.password) via nevr_config, not the game
  // JSON. Both must be present and non-empty to attach URL credentials.
  // An unset required auth.password already failed the server loud at
  // config load (service_config.cpp), so reaching here with an empty
  // password just means "no URL credentials" — we fall through to the
  // Bearer/JWT path and never put an empty secret on the wire (N115).
  // The password value is never logged.
  // Issue #41: both values are percent-encoded (ServerDbUri, the same
  // encoder the ServerDB registration URI uses), so a password with
  // '&', '=', '#', '%', '+' or whitespace reaches Nakama byte-for-byte.
  {
    const char* cfgDiscordId = NevrCfgGetFlat("nevr_discord_id");
    const char* cfgPassword = NevrCfgGetFlat("nevr_password");
    std::optional<std::string> withCredentials = nevr_serverdb_uri::BuildBridgeCredentialUri(
        remoteUrl, cfgDiscordId ? std::string_view(cfgDiscordId) : std::string_view(),
        cfgPassword ? std::string_view(cfgPassword) : std::string_view());
    if (withCredentials) {
      remoteUrl = std::move(*withCredentials);
    } else {
      // Allocation failure in the encoder: connect without URL credentials
      // (Bearer path below) rather than put an unencoded secret on the wire.
      Log(EchoVR::LogLevel::Error,
          "[NEVR.WS] conn=%d (%s) could not percent-encode URL credentials; connecting without them",
          connIdx, ConnLabel(connIdx));
    }
  }
  // Matchmaker: pnsradmatchmaking uses protobuf, not EchoVR
  // binary. Strip format=evr so the server uses default protobuf handling.
  // Issue #116: format=evr is routinely the FIRST query param here: a configured
  // socket_uri such as "wss://host/ws?format=evr&token=..." already
  // carries it before the credentials block above appends
  // discordid/password. A naive "delete the preceding
  // ? or &" deleted the URI's only '?' and glued the path to the
  // remaining query. nevr_serverdb_uri::RemoveQueryParam handles leading/
  // middle/trailing/sole position correctly; see its own tests.
  if (role == nevr_socket_role::Role::Matchmaker) {
    remoteUrl = nevr_serverdb_uri::RemoveQueryParam(remoteUrl, "format=evr");
    const std::string diagnostic = nevr_log_diagnostics::FormatRedactedUrlDiagnostic(
        "[NEVR.WS] Matchmaker conn=" + std::to_string(connIdx) + " using protobuf URL: ", remoteUrl);
    Log(EchoVR::LogLevel::Debug, "%s", diagnostic.c_str());
  }
  remote->setUrl(remoteUrl);
  remote->disableAutomaticReconnection();
  remote->disablePerMessageDeflate();

  LoginIdentity identity = ReadLoginIdentity(true);
  std::string bearerToken = std::move(identity.bearerToken);
  std::string accountName = std::move(identity.accountName);
  const uint64_t discordId = identity.discordId;
  // Only attach Bearer token if the URL doesn't already have credentials.
  // The /spr endpoint authenticates via URL query params (discordid/password).
  // Sending Bearer on top may cause the server to use the JWT session instead
  // of the URL-credential session, breaking matchmaker state.
  bool hasUrlCredentials = remoteUrl.find("discordid=") != std::string::npos;
  std::string serverKey;
  if (hasUrlCredentials) {
    const char* cfgServerKey = NevrCfgGetFlat("nevr_server_key");
    if (cfgServerKey) serverKey = cfgServerKey;
  }
  const std::string remoteBearer = nevr_evr_codec::SelectRemoteBearer(hasUrlCredentials, bearerToken, serverKey);
  // #1254: the sign-in a refusal is held against: the credential the remote presents (the server key with URL
  // credentials, else the token-auth JWT), so a changed JWT the remote never sees does not lift the latch. A client
  // with none has the one key "", because its credentials do not change within a run.
  const std::string refusalKey = remoteBearer;
  // #1446: a client login whose sign-in ended without a token is answered here (AnswerLoginLocally); the service is not
  // asked, and no remote is made. A live login session is never answered, because answering frees the slot it holds.
  // No caller holds g_pairsMutex here: the legacy Open call and the router's RemoteSockets::Open both run outside it.
  bool loginSessionLive = false;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    loginSessionLive = g_loginRemoteWs != nullptr;
  }
  if (nevr_login_refusal::AnswersLoginLocally(g_isServer != FALSE, role == nevr_socket_role::Role::Login,
                                              !remoteBearer.empty(), hasUrlCredentials, SignInEndedWithoutTokenNow(),
                                              loginSessionLive)) {
    PreparedRemote answered;
    answered.answerLocally = true;
    return answered;
  }
  if (!g_isServer && ClientRefusedAlready(refusalKey)) {
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.WS] conn=%d (%s): this sign-in was refused earlier; not connecting to the service", connIdx,
        ConnLabel(connIdx));
    return PreparedRemote{};
  }
  if (!remoteBearer.empty()) {
    ix::WebSocketHttpHeaders headers;
    headers["Authorization"] = "Bearer " + remoteBearer;
    remote->setExtraHeaders(headers);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.WS] remote auth: %s (value not logged)",
        hasUrlCredentials ? "server key + URL credentials" : "token-auth JWT");
    if (!hasUrlCredentials && nevr_evr_codec::IsBearerReplacingPath(remoteUrl)) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.WS] remote auth: token-auth JWT sent to the /ws path, whose front replaces "
          "the Bearer with the server key; the login will arrive unauthenticated. Use the "
          "/nevr ingress (services.socket_uri or the build default, #52)");
    }
  } else if (hasUrlCredentials) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.WS] remote auth: URL credentials but no server key configured — the /nevr "
        "ingress will reject the upgrade (auth.server_key, or the embedded build default)");
  } else {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.WS] remote auth: no token and no URL credentials — the session will be "
        "unauthenticated");
  }

  PreparedRemote prepared;
  prepared.remote = std::move(remote);
  prepared.refusalKey = refusalKey;
  prepared.bearerToken = std::move(bearerToken);
  prepared.accountName = std::move(accountName);
  prepared.discordId = discordId;
  return prepared;
}

// ============================================================================
// The session-router path (#708)
// ============================================================================
//
// network.session_router (flat key nevr_session_router) hands game sockets to nevr_session_router, the rules
// the Quest build ships. `config` hands it the /config sockets; login and matchmaker sockets keep the bridge's
// own rules until the later stages. One RouterPath is built per InstallWebSocketBridge and is never destroyed:
// ixwebsocket threads call into it until the process ends, and destroying a remote from its own thread joins
// itself. Stop shuts it down (Router::Shutdown ignores every event after) and stops its remotes on the caller.

static EchoVR::LogLevel RouterLogLevel(nevr_session_router::LogLevel level) {
  switch (level) {
    case nevr_session_router::LogLevel::Debug: return EchoVR::LogLevel::Debug;
    case nevr_session_router::LogLevel::Info: return EchoVR::LogLevel::Info;
    case nevr_session_router::LogLevel::Warning: return EchoVR::LogLevel::Warning;
    case nevr_session_router::LogLevel::Error: return EchoVR::LogLevel::Error;
  }
  return EchoVR::LogLevel::Info;
}

// The live shared_ptr of a game-side socket, found in the servers' client sets (they own the sockets).
static std::shared_ptr<ix::WebSocket> FindGameSocket(const ix::WebSocket* raw) {
  for (ix::WebSocketServer* server : {g_server.get(), g_matchServer.get()}) {
    if (server == nullptr) continue;
    for (const auto& client : server->getClients()) {
      if (client.get() == raw) return client;
    }
  }
  return nullptr;
}

#ifdef NEVR_TEST_HOOKS
static std::atomic<int> g_testRouterGate{-1};
static std::atomic<bool> g_testRefuseRemoteOpens{false};
#endif

// The platform code a login uses: the bridge's login platform, or DMO when the game marks this client No VR.
static uint64_t CurrentPlatformCode() {
  const char* cfgDiscordId = NevrCfgGetFlat("nevr_discord_id");
  const char* cfgPassword = NevrCfgGetFlat("nevr_password");
  const bool hasUrlCreds = cfgDiscordId && cfgDiscordId[0] && cfgPassword && cfgPassword[0];
  return nevr_evr_codec::SelectPlatformCode(hasUrlCreds, GameIsNoVr());
}

// The login the router injects (Options::buildLogin), the same bytes the legacy Open handler injects: the
// identity, the CNSUser login-state patch that lets LogInSuccessCB accept the reply, the social identity and the
// request. Nothing is returned (and an Error logged) when the request cannot be built.
static std::optional<std::string> BuildRouterLogin() {
  constexpr int kLoginConnIdx = 1;  // the login connection's number (ConnLabel names it "login")
  const LoginIdentity identity = ReadLoginIdentity(false);
  const uint64_t platformCode = CurrentPlatformCode();
  LogLoginPlatform(platformCode);
  PatchCnsUserLoginState(identity.discordId, platformCode);
  const char* cfgPassword = NevrCfgGetFlat("nevr_password");
  const std::string cfgPasswordStr = cfgPassword ? cfgPassword : "";
  g_lastInjectedDiscordId = identity.discordId;
  nevr_social_party::Global().SetSelf(identity.discordId, identity.accountName, platformCode);
  std::string loginMsg =
      BuildLoginRequest(identity.discordId, platformCode, identity.accountName, identity.bearerToken, cfgPasswordStr);
  if (loginMsg.empty()) {
    Log(EchoVR::LogLevel::Error, "[NEVR.WS] login NOT injected conn=1 (login): the request could not be built");
    return std::nullopt;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.WS] login injected xpid=%s-%llu platform=%d conn=%d (%s) size=%zu",
      nevr_evr_codec::PlatformPrefix(platformCode), static_cast<unsigned long long>(identity.discordId),
      static_cast<int>(platformCode), kLoginConnIdx, ConnLabel(kLoginConnIdx), loginMsg.size());
  return loginMsg;
}

// N92: a dedicated server has no interactive login; ServerDB answers its LoginRequest with a LoginFailure and the
// game would stall at the login gate. The router delivers this LoginSuccess in its place (Options::replaceLoginFailure).
static std::optional<std::string> ServerModeLoginSuccess() {
  if (!g_isServer) return std::nullopt;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.WS] Server mode — the service answered the server's /login with a LoginFailure; the game gets a "
      "synthesized LoginSuccess (the server's credential is checked at ServerDB registration)");
  return nevr_evr_codec::BuildLoginSuccess(CurrentPlatformCode(), g_lastInjectedDiscordId);
}

// A frame about to go to a game: the LoginFailure text is reordered so the code line comes first (the game reads
// the first line of the message).
static std::optional<std::string> RewriteFrameForGame(const std::string& frame, bool binary) {
  if (!binary || nevr_evr_codec::FirstSymbol(frame) != nevr_evr_codec::kSymLoginFailure) return std::nullopt;
  std::optional<std::string> reordered = MoveCodeLineFirst(frame);
  if (reordered.has_value()) {
    Log(EchoVR::LogLevel::Info, "[NEVR.WS] login failure text reordered: code line moved first bytes=%zu",
        reordered->size());
  }
  return reordered;
}

// What the legacy remote callback logs about a server frame, for a frame the router delivers: the numeric
// LoginFailure diagnostic (the message text can carry private data and is never logged), LOGIN SUCCESS, and the
// friend decodes.
static void LogRemoteFrameDetails(const std::string& frame) {
  const uint64_t rsym = nevr_evr_codec::FirstSymbol(frame);
  if (rsym == nevr_evr_codec::kSymLoginFailure && frame.size() > 48) {
    const std::optional<nevr_evr_codec::LoginFailure> diagnostic = nevr_evr_codec::ParseLoginFailure(frame);
    const std::string message = nevr_log_diagnostics::FormatLoginFailureDiagnostic(
        diagnostic.has_value(), diagnostic ? diagnostic->statusCode : 0, diagnostic ? diagnostic->messageBytes : 0,
        g_isServer != FALSE);
    Log(EchoVR::LogLevel::Warning, "%s", message.c_str());
  }
  if (rsym == nevr_evr_codec::kSymLoginSuccess) Log(EchoVR::LogLevel::Info, "[NEVR.WS] LOGIN SUCCESS");
  if (rsym == 0x7f197e30c72c6e61 && frame.size() >= 24 + 17) {
    uint64_t friendId = 0;
    memcpy(&friendId, frame.data() + 24 + 8, 8);
    const uint8_t statusCode = static_cast<uint8_t>(frame.data()[24 + 16]);
    Log(EchoVR::LogLevel::Warning, "[NEVR.WS] FRIEND INVITE FAILURE: friendId=%llu status=%u",
        static_cast<unsigned long long>(friendId), statusCode);
  }
  if (rsym == 0x7f0c6a3ac83c6f77 && frame.size() >= 24 + 16) {
    uint64_t friendId = 0;
    memcpy(&friendId, frame.data() + 24 + 8, 8);
    Log(EchoVR::LogLevel::Debug, "[NEVR.WS] FRIEND INVITE SUCCESS: friendId=%llu", static_cast<unsigned long long>(friendId));
  }
  if (rsym == 0xa78aeb2a4e89b10b && frame.size() >= 24 + 0x20) {
    uint32_t noff, nbusy, non;
    memcpy(&noff, frame.data() + 24 + 8, 4);
    memcpy(&nbusy, frame.data() + 24 + 12, 4);
    memcpy(&non, frame.data() + 24 + 16, 4);
    uint32_t nsent, nrecv;
    memcpy(&nsent, frame.data() + 24 + 20, 4);
    memcpy(&nrecv, frame.data() + 24 + 24, 4);
    Log(EchoVR::LogLevel::Debug, "[NEVR.WS] FRIEND LIST: online=%u busy=%u offline=%u sent=%u recv=%u", non, nbusy, noff,
        nsent, nrecv);
  }
}

static nevr_ws_transport::RemoteHooks MakeRemoteHooks() {
  nevr_ws_transport::RemoteHooks hooks;
  hooks.prepare = [](const nevr_session_router::RemoteOpenRequest& request) -> std::shared_ptr<ix::WebSocket> {
#ifdef NEVR_TEST_HOOKS
    if (g_testRefuseRemoteOpens.load()) return nullptr;
#endif
    PreparedRemote prepared = PrepareRemote(request.role, request.connIdx);
    if (prepared.remote) {
      std::lock_guard<std::mutex> lk(g_refusalMutex);
      g_routerRefusalKeys[request.remote] = prepared.refusalKey;
    }
    return prepared.remote;
  };
  hooks.refused = [](const nevr_session_router::RemoteOpenRequest& request, int httpStatus) {
    std::string key;
    {
      std::lock_guard<std::mutex> lk(g_refusalMutex);
      const auto it = g_routerRefusalKeys.find(request.remote);
      if (it != g_routerRefusalKeys.end()) {
        key = it->second;
        g_routerRefusalKeys.erase(it);  // a refused remote retires; its key is not needed again
      }
    }
    OnRemoteLoginRefused(httpStatus, key, request.connIdx);
  };
  hooks.tap = [](const nevr_session_router::RemoteOpenRequest& request, const std::string& frame) {
    ObserveSocialFrames("server->game", request.connIdx, frame);
    LogFrameMessages("server->game", request.connIdx, frame);
    LogRemoteFrameDetails(frame);
  };
  hooks.guard = [](std::function<void(const ix::WebSocketMessagePtr&)> fn) {
    return std::function<void(const ix::WebSocketMessagePtr&)>(GuardWsCallback("ws_bridge.cpp:routerRemote", std::move(fn)));
  };
  hooks.log = [](nevr_session_router::LogLevel level, const std::string& line) {
    Log(RouterLogLevel(level), "[NEVR.WS] %s", line.c_str());
  };
  return hooks;
}

// The one line of router counters, at every session end and at stop (a Warning when a drop counter moved since
// the last line). `why` names the moment.
struct RouterPath;
static void LogRouterStats(RouterPath& routed, const char* why);

struct RouterPath {
  explicit RouterPath(nevr_session_router_mode::Mode m)
      : mode(m), games(&FindGameSocket), remotes(MakeRemoteHooks()), router(&games, &remotes, MakeOptions(m, this)) {
    remotes.SetRouter(&router);
    games.SetOutboundRewrite(&RewriteFrameForGame);
  }
  static nevr_session_router::Options MakeOptions(nevr_session_router_mode::Mode m, RouterPath* self) {
    nevr_session_router::Options options;
    options.log = [](nevr_session_router::LogLevel level, const std::string& line) {
      Log(RouterLogLevel(level), "[NEVR.WS] %s", line.c_str());
    };
    if (m == nevr_session_router_mode::Mode::All) {
#ifdef NEVR_TEST_HOOKS
      options.loginGate = []() {
        switch (g_testRouterGate.load()) {
          case 0: return nevr_session_router::LoginGate::Awaiting;
          case 2: return nevr_session_router::LoginGate::Refused;
          default: return nevr_session_router::LoginGate::Ready;
        }
      };
#endif
      options.buildLogin = &BuildRouterLogin;
      // A dedicated server is not a player: it keeps no friend list (#1280). g_isServer is set before the bridge
      // starts (boot.cpp).
      options.subscribeFriendList = g_isServer == FALSE;
      options.replaceLoginFailure = &ServerModeLoginSuccess;
      options.onSessionEnded = [self]() {
        bool wasLoginSession;
        {
          std::lock_guard<std::mutex> lk(g_pairsMutex);
          wasLoginSession = g_roles.LoginClaimed();
          g_roles.LoginSessionEnded();
        }
        {
          RemoteSessionEndingScope ending;  // runs on the ended session's own callback thread: send nothing on it
          nevr_self_check::SetLoggedIn(false);  // no result is sent until the next LoginSuccess
        }
        if (wasLoginSession) {
          Log(EchoVR::LogLevel::Info,
              "[NEVR.WS] login session ended: the game's next login socket is a new login (shared connections renumbered from 2)");
        }
        LogRouterStats(*self, "session ended");
      };
    }
    return options;
  }
  nevr_session_router_mode::Mode mode;
  nevr_ws_transport::GameSockets games;
  nevr_ws_transport::RemoteSockets remotes;
  nevr_session_router::Router router;
  nevr_session_router::Stats lastStats;
  std::mutex statsMutex;
};

static std::atomic<RouterPath*> g_routerPath{nullptr};

static bool RouterOwnsLogin() {
  const RouterPath* routed = g_routerPath.load();
  return routed != nullptr && nevr_session_router_mode::RoutesRole(routed->mode, nevr_socket_role::Role::Login);
}

static bool RouterSendToSession(const std::string& frame) {
  RouterPath* routed = g_routerPath.load();
  return routed != nullptr && routed->router.SendToSession(frame);
}

// A frame delivered as if the service had sent it: through the tap (the observers and logs) and the router's
// own routing for the session remote.
[[maybe_unused]] static bool RouterInjectServerFrame(const std::string& frame) {
  RouterPath* routed = g_routerPath.load();
  if (routed == nullptr) return false;
  const std::optional<nevr_session_router::RemoteId> session = routed->router.SessionRemote();
  if (!session) return false;
  ObserveSocialFrames("server->game", 1, frame);
  LogFrameMessages("server->game", 1, frame);
  routed->router.OnRemoteFrame(*session, frame, true);
  return true;
}

static void LogRouterStats(RouterPath& routed, const char* why) {
  const nevr_session_router::Stats now = routed.router.GetStats();
  bool dropMoved;
  {
    std::lock_guard<std::mutex> lk(routed.statsMutex);
    const nevr_session_router::Stats& before = routed.lastStats;
    dropMoved = now.droppedGameFrames != before.droppedGameFrames || now.droppedRemoteFrames != before.droppedRemoteFrames ||
                now.droppedUnrequires != before.droppedUnrequires ||
                now.configRepliesOrphaned != before.configRepliesOrphaned ||
                now.configRepliesUnmatched != before.configRepliesUnmatched ||
                now.configRequestsEvicted != before.configRequestsEvicted;
    routed.lastStats = now;
  }
  Log(dropMoved ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
      "[NEVR.WS] router stats (%s) games=%llu remotes=%llu dropped_game=%llu dropped_remote=%llu dropped_unrequires=%llu "
      "unmatched_embedded=%llu config_unmatched=%llu config_orphaned=%llu config_evicted=%llu",
      why, static_cast<unsigned long long>(now.games), static_cast<unsigned long long>(now.remotes),
      static_cast<unsigned long long>(now.droppedGameFrames), static_cast<unsigned long long>(now.droppedRemoteFrames),
      static_cast<unsigned long long>(now.droppedUnrequires), static_cast<unsigned long long>(now.unmatchedEmbeddedUnrequires),
      static_cast<unsigned long long>(now.configRepliesUnmatched), static_cast<unsigned long long>(now.configRepliesOrphaned),
      static_cast<unsigned long long>(now.configRequestsEvicted));
}


// ============================================================================
// Public API
// ============================================================================

void SetWebSocketBridgeTarget(const char* uri) {
  g_remoteUri = uri;
}

uint16_t GetWebSocketBridgePort() {
  return g_proxyPort;
}

uint16_t GetMatchmakerBridgePort() {
  return g_matchPort;
}

bool IsWebSocketBridgeActive() {
  return g_bridgeEnabled;
}

void InstallWebSocketBridge() {
  // Currently unreachable: InstallWebSocketBridge() has exactly one caller
  // (boot.cpp), and that caller already gates the call behind a non-empty
  // socketUri, logging its own WARNING and returning before ever reaching
  // here if it's empty. Left in place as defense-in-depth for any future
  // caller (tests, a CLI reconfigure path) that calls this without that gate.
  if (g_remoteUri.empty()) {
    Log(EchoVR::LogLevel::Info, "[NEVR.WS] No wss:// target — bridge disabled");
    return;
  }

  WireSelfChecks();

  // #708: which sockets the router takes. A value that names no mode (or one this build cannot run) is fatal at
  // boot: a typo that silently ran the legacy routing would be a test that proves nothing about the router.
  {
    std::string problem;
    const nevr_session_router_mode::Mode mode =
        nevr_session_router_mode::ParseMode(NevrCfgGetFlat("nevr_session_router"), &problem);
    if (!problem.empty()) {
      Log(EchoVR::LogLevel::Error, "[NEVR.WS] network.session_router refused: %s", problem.c_str());
      FatalError(problem.c_str(), "network.session_router");
      return;
    }
    Log(EchoVR::LogLevel::Info, "[NEVR.WS] network.session_router=%s", nevr_session_router_mode::ModeName(mode));
    // A fresh router per install: a stopped one has shut down for good. The old one is leaked on purpose (see
    // RouterPath); a bridge is installed once per process outside tests.
    g_routerPath.store(mode == nevr_session_router_mode::Mode::Legacy ? nullptr : new RouterPath(mode));
  }

  // One-time WSA init
  static bool netInit = false;
  if (!netInit) { ix::initNetSystem(); netInit = true; }

  // Bind to a high-range ephemeral port with retry.
  // Port 6821 is permanently poisoned on this host (see N37/N39).
  constexpr int kMaxBindAttempts = 10;
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint16_t> dist(49152, 65535);

  bool bound = false;
  for (int attempt = 0; attempt < kMaxBindAttempts; ++attempt) {
    uint16_t tryPort = dist(gen);
    g_server = std::make_unique<ix::WebSocketServer>(tryPort, "127.0.0.1");
    g_server->disablePerMessageDeflate();

    auto [ok, errorText] = g_server->listen();
    if (ok) {
      g_proxyPort = tryPort;
      bound = true;
      break;
    }

    const std::string diagnostic = nevr_log_diagnostics::FormatBindFailureDiagnostic(
        "Proxy", tryPort, attempt + 1, kMaxBindAttempts);
    Log(EchoVR::LogLevel::Warning, "%s error=\"%s\"", diagnostic.c_str(), errorText.c_str());
    g_server.reset();
  }

  if (!bound) {
    g_server.reset();
    char errBuf[128];
    snprintf(errBuf, sizeof(errBuf),
             "WebSocket bridge: failed to bind any port after %d attempts",
             kMaxBindAttempts);
    FatalError(errBuf, "ws_bridge bind failure");
    return;
  }

  // Callbacks set after successful listen(), before start(). One handler per listener: the listener a
  // socket arrives on is part of what decides its role (#520).
  const auto makeOnClientMessage = [](nevr_socket_role::Listener listener) {
    return GuardWsCallback("ws_bridge.cpp:setOnClientMessageCallback",
      [listener](std::shared_ptr<ix::ConnectionState> connState,
         ix::WebSocket& gameWs,
         const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
          case ix::WebSocketMessageType::Open: {
            // #520: the role comes from the request target the socket asked for (the redirect gives each
            // service its own), then the listener, and only then the connect order. The login session is
            // read in the same critical section that takes the slot: the Open handlers of two sockets
            // run on different threads.
            const AdmittedSocket admitted = AdmitGameSocket(&gameWs, msg->openInfo.uri, listener);
            const int connIdx = admitted.admission.connIdx;
            const nevr_socket_role::Role role = admitted.admission.role;
            const std::shared_ptr<ix::WebSocket>& loginRemote = admitted.loginRemote;

            // #708: a socket whose role the router takes is registered with it and gets no ProxyPair; the
            // router opens, shares and ends the remote session. The role is the bridge's decision (hinted).
            if (RouterPath* routed = g_routerPath.load(); routed != nullptr &&
                nevr_session_router_mode::RoutesRole(routed->mode, role)) {
              const nevr_session_router::GameId id = routed->games.Register(&gameWs, connIdx);
              LogAdmission(admitted, &gameWs, msg->openInfo.uri, listener, "router", id);
              routed->router.OnGameOpen(id, nevr_session_router::OpenHint{role, true});
              Log(EchoVR::LogLevel::Info, "[NEVR.WS] Proxy: game connected (conn=%d, %s) path=router game=%llu",
                  connIdx, ConnLabel(connIdx), static_cast<unsigned long long>(id));
              break;
            }
            LogAdmission(admitted, &gameWs, msg->openInfo.uri, listener, "legacy", 0);

            // A socket that shares the login session (the matchmaker, or a further /login): reuse the
            // login connection's remote WS.
            // The matchmaker needs the fully-authenticated session (login + profile
            // exchange) that conn=1 established. A fresh LoginRequest-only session
            // won't have the server-side state needed for PlayerSessionRequest.
            // Don't inject LoginRequest — the session is already logged in.
            if (role == nevr_socket_role::Role::Matchmaker && loginRemote) {
              Log(EchoVR::LogLevel::Info,
                  "[NEVR.WS] Proxy: game connected (conn=%d, %s), sharing login session (no LoginRequest)",
                  connIdx, ConnLabel(connIdx));
              auto pair = std::make_unique<ProxyPair>();
              pair->remoteWs = loginRemote;
              pair->remoteOpen = true;
              pair->loginInjected = true;  // skip LoginRequest — already authenticated
              pair->connIdx = connIdx;
              pair->role = role;
              pair->listener = listener;
              pair->gameWs = FindGameSocket(&gameWs);

              auto* pairPtr = pair.get();
              ix::WebSocket* gameWsPtr = &gameWs;

              // N61: register an independent callback for each matchmaker
              // connection on the shared remote. Relying on the login
              // connection's callback alone fails: when login
              // disconnects and B2/N54 nulls that callback, all matchmaker
              // server→game message routing silently dies.
              loginRemote->setOnMessageCallback(GuardWsCallback("ws_bridge.cpp:setOnMessageCallback",
                  [pairPtr, gameWsPtr, connIdx,
                   remoteAddress = static_cast<const ix::WebSocket*>(loginRemote.get())](const ix::WebSocketMessagePtr& rmsg) {
                    switch (rmsg->type) {
                      case ix::WebSocketMessageType::Message: {
                        ix::WebSocket* target = nullptr;
                        std::shared_ptr<ix::WebSocket> keep;  // holds the target alive across the send
                        int targetConn = -1;
                        {
                          std::lock_guard<std::mutex> lk(g_pairsMutex);
                          if (const ProxyPair* route = SharedRouteLocked(&target)) {
                            targetConn = route->connIdx;
                            keep = route->gameWs.lock();
                          }
                        }
                        ObserveSocialFrames("server->game", targetConn, rmsg->str);
                        if (!keep) target = nullptr;  // the server already erased this game socket
                        if (target == nullptr) {
                          LogSharedFrameDropped(rmsg->str.size());
                          break;
                        }
                        NEVR_BEFORE_SHARED_SEND(target);
                        if (rmsg->binary) {
                          keep->sendBinary(rmsg->str);
                        } else {
                          keep->sendText(rmsg->str);
                        }
                        if (keep && keep.use_count() == 1) { ReleaseOffThread(std::move(keep)); }
                        break;
                      }
                      case ix::WebSocketMessageType::Close:
                        Log(EchoVR::LogLevel::Info,
                            "[NEVR.WS] Remote closed (conn=%d, %s): code=%u",
                            connIdx, ConnLabel(connIdx), static_cast<unsigned int>(rmsg->closeInfo.code));
                        CloseGameSocketsForRemote(remoteAddress, connIdx,
                                                  static_cast<unsigned int>(rmsg->closeInfo.code));
                        break;
                      default:
                        break;
                    }
                  }));

              {
                std::lock_guard<std::mutex> lk(g_pairsMutex);
                g_activeGameWs = &gameWs;
                g_pairs[&gameWs] = std::move(pair);
              }
              break;
            }

            // A config or login socket, or a matchmaker with no login session to share: create a new remote ws
            PreparedRemote prepared = PrepareRemote(role, connIdx);
            if (prepared.answerLocally) {
              AnswerLoginLocally(&gameWs, connIdx);
              break;
            }
            if (!prepared.remote) {
              AbandonGameSocket(&gameWs, role, connIdx);
              break;
            }
            const std::shared_ptr<ix::WebSocket> remote = prepared.remote;
            const std::string& bearerToken = prepared.bearerToken;
            const std::string& accountName = prepared.accountName;
            const uint64_t discordId = prepared.discordId;

            auto pair = std::make_unique<ProxyPair>();
            pair->remoteWs = remote;
            pair->connIdx = connIdx;
            pair->role = role;
            pair->listener = listener;
            pair->gameWs = FindGameSocket(&gameWs);

            auto* pairPtr = pair.get();
            ix::WebSocket* gameWsPtr = &gameWs;

            // Remote → game forwarding
            remote->setOnMessageCallback(GuardWsCallback("ws_bridge.cpp:setOnMessageCallback", 
                // accountName captured BY VALUE alongside discordId — this callback
                // outlives the enclosing scope, so a reference would dangle (N123).
                // remoteAddress: only compared, never dereferenced (capturing the shared_ptr would be a cycle).
                [pairPtr, gameWsPtr, connIdx, discordId, accountName, bearerToken, refusalKey = prepared.refusalKey,
                 remoteAddress = static_cast<const ix::WebSocket*>(remote.get())](const ix::WebSocketMessagePtr& rmsg) {
                  switch (rmsg->type) {
                    case ix::WebSocketMessageType::Open:
                      HandleRemoteOpen(pairPtr, connIdx, discordId, accountName, bearerToken);
                      break;
                    case ix::WebSocketMessageType::Message: {
                      ObserveSocialFrames("server->game", connIdx, rmsg->str);
                      // First message's symbol (marker@0, symbol@8, length@16), for the decodes below.
                      const uint64_t rsym = nevr_evr_codec::FirstSymbol(rmsg->str);
                      LogFrameMessages("server->game", connIdx, rmsg->str);
                      // Decode only the numeric LoginFailure diagnostics. The
                      // server-provided message can contain credentials or other
                      // private response data and is never written to logs.
                      if (rsym == nevr_evr_codec::kSymLoginFailure && rmsg->str.size() > 48) {
                        const std::optional<nevr_evr_codec::LoginFailure> diagnostic =
                            nevr_evr_codec::ParseLoginFailure(rmsg->str);
                        const std::string message = nevr_log_diagnostics::FormatLoginFailureDiagnostic(
                            diagnostic.has_value(), diagnostic ? diagnostic->statusCode : 0,
                            diagnostic ? diagnostic->messageBytes : 0, g_isServer != FALSE);
                        Log(EchoVR::LogLevel::Warning, "%s", message.c_str());

                        // N92: ported from the ws-bridge module, which was the shipping
                        // copy until the monolithic fold. A dedicated server has no
                        // interactive login; ServerDB answers LoginRequest with
                        // LoginFailure, and without this the game stalls at the login
                        // gate. Synthesize the LoginSuccess the game is waiting for and
                        // do NOT forward the failure.
                        if (g_isServer) {
                          Log(EchoVR::LogLevel::Info,
                              "[NEVR.WS] Server mode — the service answered the server's /login with a LoginFailure; "
                              "the game gets a synthesized LoginSuccess (the server's credential is checked at "
                              "ServerDB registration)");
                          uint64_t serverPlatformCode;
                          {
                            const char* cfgDiscordId2 = NevrCfgGetFlat("nevr_discord_id");
                            const char* cfgPassword2 = NevrCfgGetFlat("nevr_password");
                            bool hasUrlCreds2 = cfgDiscordId2 && cfgDiscordId2[0] && cfgPassword2 && cfgPassword2[0];
                            serverPlatformCode = nevr_evr_codec::SelectPlatformCode(hasUrlCreds2, GameIsNoVr());
                          }
                          const std::string fakeSuccess =
                              nevr_evr_codec::BuildLoginSuccess(serverPlatformCode, g_lastInjectedDiscordId);
                          std::shared_ptr<ix::WebSocket> keep;  // holds the game socket alive across the send
                          {
                            std::lock_guard<std::mutex> lk(g_pairsMutex);
                            const auto pit = g_pairs.find(gameWsPtr);
                            if (pit != g_pairs.end()) keep = pit->second->gameWs.lock();
                          }
                          if (!keep) {
                            LogSharedFrameDropped(fakeSuccess.size());
                            break;
                          }
                          NEVR_BEFORE_SHARED_SEND(keep.get());
                          keep->sendBinary(fakeSuccess);
                          if (keep.use_count() == 1) { ReleaseOffThread(std::move(keep)); }
                          break;  // failure is not forwarded to the game
                        }
                      }
                      // Decode LoginSuccess (sym 0xa5acc1a90d0cce47)
                      if (rsym == nevr_evr_codec::kSymLoginSuccess) {
                        Log(EchoVR::LogLevel::Info, "[NEVR.WS] LOGIN SUCCESS");
                        // Inject SNSFriendListSubscribeRequest directly to server.
                        // pnsrad's broadcaster handle (field_0x160) is null because
                        // echovr.exe doesn't provide one for the pnsrad platform
                        // provider, so pnsrad can't send SNS messages itself. Send
                        // the subscribe request through the WS bridge instead.
                        // A dedicated server is not a player: it keeps no friend list (#1280).
                        if (!g_isServer) {
                          // Payload: 0x20 zero bytes (provider_id + UUID + token); the server ignores it.
                          const std::string subscribeMsg = nevr_evr_codec::BuildFriendListSubscribe();
                          // pairPtr is not read here: g_pairs owns the pair and a closing game socket erases it under
                          // g_pairsMutex (#752). The pair is found by its game socket, as ForwardGameFrameToRemote does,
                          // and the send runs outside the mutex: a failed send runs the remote's Close callback on this
                          // thread, and that callback takes the mutex (#738).
                          std::shared_ptr<ix::WebSocket> subscribeRemote;
                          {
                            std::lock_guard<std::mutex> lk(g_pairsMutex);
                            auto it = g_pairs.find(gameWsPtr);
                            if (it != g_pairs.end()) subscribeRemote = it->second->remoteWs;
                          }
                          if (subscribeRemote) {
                            subscribeRemote->sendBinary(subscribeMsg);
                            Log(EchoVR::LogLevel::Debug,
                                "[NEVR.WS] Injected FriendListSubscribeRequest (%zu bytes)",
                                subscribeMsg.size());
                          } else {
                            Log(EchoVR::LogLevel::Debug,
                                "[NEVR.WS] FriendListSubscribeRequest not sent: game socket %d has no pair",
                                connIdx);
                          }
                        }
                      }
                      // STcpConnectionUnrequireEvent is deliberately NOT acted on.
                      // d0190c4/dd1e9e7 closed the remote here in server mode; Nakama sends the event on
                      // the config connection too, so the config socket died before the game's post-login
                      // config requests and the server never left "logging in" (real Windows 2026-10-01
                      // 02:17Z; test_bridge_never_closes_a_remote_on_unrequire).
                      // Decode SNS friend messages
                      // InviteFailure (0x7f197e30c72c6e61): Header(8)+FriendID(8)+StatusCode(1)
                      if (rsym == 0x7f197e30c72c6e61 && rmsg->str.size() >= 24 + 17) {
                        uint64_t friendId = 0;
                        uint8_t statusCode = 0;
                        memcpy(&friendId, rmsg->str.data() + 24 + 8, 8);
                        statusCode = static_cast<uint8_t>(rmsg->str.data()[24 + 16]);
                        Log(EchoVR::LogLevel::Warning,
                            "[NEVR.WS] FRIEND INVITE FAILURE: friendId=%llu status=%u",
                            static_cast<unsigned long long>(friendId), statusCode);
                      }
                      // InviteSuccess (0x7f0c6a3ac83c6f77): Header(8)+FriendID(8)
                      if (rsym == 0x7f0c6a3ac83c6f77 && rmsg->str.size() >= 24 + 16) {
                        uint64_t friendId = 0;
                        memcpy(&friendId, rmsg->str.data() + 24 + 8, 8);
                        Log(EchoVR::LogLevel::Debug,
                            "[NEVR.WS] FRIEND INVITE SUCCESS: friendId=%llu",
                            static_cast<unsigned long long>(friendId));
                      }
                      // FriendListResponse (0xa78aeb2a4e89b10b): counts + per-friend entries
                      if (rsym == 0xa78aeb2a4e89b10b && rmsg->str.size() >= 24 + 0x20) {
                        uint32_t noff, nbusy, non, nsent, nrecv;
                        memcpy(&noff, rmsg->str.data() + 24 + 8, 4);
                        memcpy(&nbusy, rmsg->str.data() + 24 + 12, 4);
                        memcpy(&non, rmsg->str.data() + 24 + 16, 4);
                        memcpy(&nsent, rmsg->str.data() + 24 + 20, 4);
                        memcpy(&nrecv, rmsg->str.data() + 24 + 24, 4);
                        Log(EchoVR::LogLevel::Debug,
                            "[NEVR.WS] FRIEND LIST: online=%u busy=%u offline=%u sent=%u recv=%u",
                            non, nbusy, noff, nsent, nrecv);
                        // Hex dump full payload for friend entry analysis
                        size_t payloadLen = rmsg->str.size() - 24;
                        const uint8_t* pp = reinterpret_cast<const uint8_t*>(rmsg->str.data()) + 24;
                        char hex[4096] = {};
                        int hoff = 0;
                        for (size_t i = 0; i < payloadLen && hoff < 4000; i++) {
                          hoff += snprintf(hex + hoff, sizeof(hex) - hoff, "%02x ", pp[i]);
                        }
                        Log(EchoVR::LogLevel::Debug, "[NEVR.WS] FRIEND payload (%zu bytes): %s",
                            payloadLen, hex);
                      }
                      // Route to the active game WS. When matchmaker (conn>=2)
                      // shares the login remote, g_activeGameWs is swapped so
                      // responses reach the matchmaker's game WS peer.
                      {
                        // The login remote is shared with the matchmaker connections: its frames go
                        // to the live connection SharedRouteLocked picks. Any other remote (config)
                        // belongs to its own game socket.
                        // pairPtr is not read here either (#752): a login game socket that closed while a matchmaker
                        // still shares this remote has erased its pair, and the callback stays installed.
                        ix::WebSocket* target = nullptr;
                        std::shared_ptr<ix::WebSocket> keep;  // holds the target alive across the send
                        {
                          std::lock_guard<std::mutex> lk(g_pairsMutex);
                          const auto pit = g_pairs.find(gameWsPtr);
                          if (pit != g_pairs.end()) {
                            // The pair is present, so its game socket has not closed and gameWsPtr is alive.
                            if (pit->second->remoteWs != nullptr && pit->second->remoteWs == g_loginRemoteWs) {
                              if (const ProxyPair* route = SharedRouteLocked(&target)) keep = route->gameWs.lock();
                            } else {
                              target = gameWsPtr;
                              keep = pit->second->gameWs.lock();
                            }
                          } else if (remoteAddress == g_loginRemoteWs.get()) {
                            // The login game socket closed and its pair is gone, but the shared login remote still
                            // carries this callback: route to the live matchmaker connection. gameWsPtr is not used.
                            if (const ProxyPair* route = SharedRouteLocked(&target)) keep = route->gameWs.lock();
                          }
                          // Otherwise the game socket closed and nothing shares this remote: target stays null, and
                          // the frame is dropped below. gameWsPtr is never used without its pair.
                        }
                        if (!keep) target = nullptr;  // the server already erased this game socket
                        if (target == nullptr) {
                          LogSharedFrameDropped(rmsg->str.size());
                          break;
                        }
                        std::optional<std::string> reordered;
                        if (rmsg->binary && rsym == nevr_evr_codec::kSymLoginFailure) {
                          reordered = MoveCodeLineFirst(rmsg->str);
                          if (reordered.has_value()) {
                            Log(EchoVR::LogLevel::Info,
                                "[NEVR.WS] login failure text reordered: code line moved first bytes=%zu",
                                reordered->size());
                          }
                        }
                        const std::string& outFrame = reordered.has_value() ? *reordered : rmsg->str;
                        NEVR_BEFORE_SHARED_SEND(target);
                        if (rmsg->binary) {
                          keep->sendBinary(outFrame);
                        } else {
                          keep->sendText(outFrame);
                        }
                        if (keep && keep.use_count() == 1) { ReleaseOffThread(std::move(keep)); }
                      }
                      break;
                    }
                    case ix::WebSocketMessageType::Close:
                      Log(EchoVR::LogLevel::Info,
                          "[NEVR.WS] Remote closed (conn=%d, %s): code=%u",
                          connIdx, ConnLabel(connIdx), static_cast<unsigned int>(rmsg->closeInfo.code));
                      // Not gameWsPtr->close() here: it deadlocks (blocks waiting for the server
                      // thread, which may be blocked on g_pairsMutex). The game sockets on this
                      // session are closed off this thread instead (#70).
                      CloseGameSocketsForRemote(remoteAddress, connIdx,
                                                static_cast<unsigned int>(rmsg->closeInfo.code));
                      break;
                    case ix::WebSocketMessageType::Error:
                      Log(EchoVR::LogLevel::Warning,
                          "[NEVR.WS] Remote error: http_status=%d retries=%u",
                          rmsg->errorInfo.http_status, rmsg->errorInfo.retries);
                      OnRemoteLoginRefused(rmsg->errorInfo.http_status, refusalKey, connIdx);
                      // Automatic reconnection is off, so a remote that fails (often its first connect,
                      // with no Close to follow) is dead; end the game's sockets on it so the game
                      // retries rather than waiting on a session that will never open (#70).
                      CloseGameSocketsForRemote(remoteAddress, connIdx, 0);
                      break;
                    default:
                      break;
                  }
                }));

            std::shared_ptr<ix::WebSocket> previousLogin;  // released after g_pairsMutex: see ReleaseOffThread (#751)
            {
              std::lock_guard<std::mutex> lk(g_pairsMutex);
              g_pairs[gameWsPtr] = std::move(pair);
              // Save the login connection for reuse by the sockets that share its session
              if (role == nevr_socket_role::Role::Login) {
                previousLogin = std::move(g_loginRemoteWs);
                g_loginRemoteWs = remote;
                g_activeGameWs = gameWsPtr;
                g_loginGameWs = gameWsPtr;
              }
            }
            ReleaseOffThread(std::move(previousLogin));
            // Start after insertion so the remote callback can find the pair in g_pairs
            remote->start();
            Log(EchoVR::LogLevel::Info, "[NEVR.WS] Proxy: game connected (conn=%d, %s)", connIdx,
                ConnLabel(connIdx));
            const std::string remoteDiagnostic =
                nevr_log_diagnostics::FormatRedactedUrlDiagnostic("[NEVR.WS] Proxy remote target: ", g_remoteUri);
            Log(EchoVR::LogLevel::Info, "%s", remoteDiagnostic.c_str());
            break;
          }

          case ix::WebSocketMessageType::Message: {
            RouterPath* routed = g_routerPath.load();
            const std::optional<nevr_session_router::GameId> routedId =
                routed != nullptr ? routed->games.IdOf(&gameWs) : std::nullopt;
            const int logConn = routedId ? routed->games.ConnIdxOf(&gameWs) : ConnIdxOfGameWs(&gameWs);
            ObserveSocialFrames("game->server", logConn, msg->str);
            LogFrameMessages("game->server", logConn, msg->str);
            // Game→remote forwarding — dump all message symbols in the frame
            // EchoVR wire format: [marker(8)][symbol(8)][length(8)][payload(length)]...
            LogGameToServerFrame(msg->str, connState->getId());
            if (routedId) {
              routed->router.OnGameFrame(*routedId, msg->str, msg->binary);
              break;
            }
            ForwardGameFrameToRemote(gameWs, msg->str, msg->binary);
            break;
          }

          case ix::WebSocketMessageType::Close: {
            if (RouterPath* routed = g_routerPath.load(); routed != nullptr) {
              if (const std::optional<nevr_session_router::GameId> id = routed->games.IdOf(&gameWs)) {
                const int closedConn = routed->games.ConnIdxOf(&gameWs);
                routed->games.Forget(&gameWs);
                routed->router.OnGameClose(*id);
                Log(EchoVR::LogLevel::Info, "[NEVR.WS] Proxy: game disconnected (conn=%d, %s) path=router game=%llu",
                    closedConn, ConnLabel(closedConn), static_cast<unsigned long long>(*id));
                break;
              }
            }
            // N60: snapshot the ProxyPair's remoteWs under the lock, then
            // release the lock BEFORE calling stop(). stop() blocks until the
            // remote thread exits, and the remote callback may be waiting on
            // g_pairsMutex (Open handler line 328, Message handler line 497).
            // Holding the mutex across stop() → ABBA deadlock.
            std::shared_ptr<ix::WebSocket> remoteToStop;
            int closedConnIdx = -1;
            {
              std::lock_guard<std::mutex> lk(g_pairsMutex);
              bool callbackCleared = false;
              remoteToStop = RetireGameWsLocked(&gameWs, &closedConnIdx, &callbackCleared);
            } // g_pairsMutex RELEASED here — safe to call stop()
            if (remoteToStop) {
              remoteToStop->stop();
            }
            Log(EchoVR::LogLevel::Info,
                "[NEVR.WS] Proxy: game disconnected (conn=%d, %s)",
                closedConnIdx, ConnLabel(closedConnIdx));
            break;
          }

          default:
            break;
        }
      });
  };

  g_server->setOnClientMessageCallback(makeOnClientMessage(nevr_socket_role::Listener::Service));
  g_server->start();
  g_bridgeEnabled = true;

  const std::string localUri = "ws://127.0.0.1:" + std::to_string(g_proxyPort);
  const std::string diagnostic = nevr_log_diagnostics::FormatRedactedUrlPairDiagnostic(
      "[NEVR.WS] Proxy listening on ", localUri, " -> ", g_remoteUri);
  Log(EchoVR::LogLevel::Info, "%s", diagnostic.c_str());

  // N146: pnsradmatchmaking uses Rad's R14NETCLIENT with a hardcoded
  // fallback host/port (matchmaker.readyatdawn.com) when matchingservice_host
  // has no explicit port. This connection bypasses our JsonValueAsStringHook,
  // so we patch pnsradmatchmaking.dll's compiled-in string directly instead
  // (PatchMatchmakingHost, pnsrad_enabler.cpp) to point at whatever port we
  // bind here — GetMatchmakerBridgePort() is how that patch learns the port.
  //
  // 2026-09-13 (Andrew): was a hardcoded port 42148. Static ports collide
  // with a still-releasing socket from a just-killed prior instance (the
  // OS hasn't finished TIME_WAIT teardown yet) — hit this live. Same
  // random-ephemeral-with-retry pattern as the main bridge port above,
  // instead of a fixed number.
  //
  // CONFESSION: this doesn't reuse `gen`/`dist` from above (each are
  // function-scoped to the block above) and doesn't exclude g_proxyPort
  // from the draw — a same-port draw just fails to bind and the loop
  // retries, so it's harmless, but it means two independent RNG streams
  // rather than one shared one. Not worth a shared-state refactor for two
  // call sites; flagging instead of silently living with it unremarked.
  {
    constexpr int kMaxMatchBindAttempts = 10;
    std::random_device matchRd;
    std::mt19937 matchGen(matchRd());
    std::uniform_int_distribution<uint16_t> matchDist(49152, 65535);

    bool matchBound = false;
    for (int attempt = 0; attempt < kMaxMatchBindAttempts; ++attempt) {
      uint16_t tryPort = matchDist(matchGen);
      g_matchServer = std::make_unique<ix::WebSocketServer>(tryPort, "127.0.0.1");
      g_matchServer->disablePerMessageDeflate();
      auto [ok, errorText] = g_matchServer->listen();
      if (ok) {
        g_matchPort = tryPort;
        matchBound = true;
        break;
      }
      const std::string diagnostic =
          nevr_log_diagnostics::FormatBindFailureDiagnostic("Matchmaker", tryPort, attempt + 1,
                                                      kMaxMatchBindAttempts);
      Log(EchoVR::LogLevel::Warning, "%s error=\"%s\"", diagnostic.c_str(), errorText.c_str());
      g_matchServer.reset();
    }

    if (matchBound) {
      g_matchServer->setOnClientMessageCallback(makeOnClientMessage(nevr_socket_role::Listener::Matchmaker));
      g_matchServer->start();
      Log(EchoVR::LogLevel::Info,
          "[NEVR.WS] Matchmaker listener on ws://127.0.0.1:%u", g_matchPort);
    } else {
      g_matchServer.reset();
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.WS] Matchmaker listener FAILED after %d attempts — matchmaking will fail",
          kMaxMatchBindAttempts);
    }
  }
}

void ShutdownWebSocketBridge() {
  g_bridgeEnabled = false;
  // Do NOT call g_server->stop() — runs under loader lock during DLL_PROCESS_DETACH.
  // Thread joins can deadlock. OS reclaims everything on process exit.
  // The graceful path uses StopWebSocketBridgeListener() instead; see below.
}

// Stop the listener from console-control shutdown or
// GameServerLib::BeginGracefulShutdown, outside the loader lock.
//
// Stopping the listener is what releases the socket FD. ixwebsocket never sets
// SO_REUSEADDR (N37), so a leaked LISTEN socket is precisely the zombie-bind
// condition N39's random-ephemeral-port retry exists to route around — that
// workaround has been carrying the whole load alone.
void StopWebSocketBridgeListener() {
  g_bridgeEnabled = false;

  // #723/#729: every stage that joins a thread logs its begin and its done with the elapsed time, so a stop
  // that never returns names the stage it is stuck in (the last "begin" with no "done").
  const ULONGLONG startedMs = GetTickCount64();
  const auto stage = [startedMs](const char* name, const char* phase, std::size_t count) {
    Log(EchoVR::LogLevel::Info, "[NEVR.WS] listener stop stage=%s %s count=%zu thread=%lu elapsed_ms=%llu", name, phase,
        count, static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long long>(GetTickCount64() - startedMs));
  };

  // N60: collect under the lock, stop OUTSIDE it. stop() joins the callback
  // thread, which may itself be waiting on g_pairsMutex — holding it here
  // deadlocks shutdown.
  std::vector<std::shared_ptr<ix::WebSocket>> remotes;
  std::size_t stoppedRouterRemotes = 0;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    for (auto& pair : g_pairs) {
      if (pair.second && pair.second->remoteWs) remotes.push_back(pair.second->remoteWs);
    }
    g_pairs.clear();
    g_activeGameWs = nullptr;
  }
  stage("legacy_remotes", "begin", remotes.size());
  for (auto& ws : remotes) {
    ws->stop();
  }
  stage("legacy_remotes", "done", remotes.size());
  if (RouterPath* routed = g_routerPath.load(); routed != nullptr) {
    // Remotes first: once stopped, their events no longer reach the router, so Shutdown's closes race nothing.
    stage("router_remotes", "begin", 0);
    stoppedRouterRemotes = routed->remotes.StopAll();
    stage("router_remotes", "done", stoppedRouterRemotes);
    stage("router_shutdown", "begin", 0);
    routed->router.Shutdown();
    stage("router_shutdown", "done", 0);
    LogRouterStats(*routed, "stop");
  }

  if (g_server) {
    stage("game_listener", "begin", 1);
    g_server->stop();
    g_server.reset();
    stage("game_listener", "done", 1);
  }
  if (g_matchServer) {
    stage("matchmaker_listener", "begin", 1);
    g_matchServer->stop();
    g_matchServer.reset();
    stage("matchmaker_listener", "done", 1);
  }
  g_proxyPort = 0;
  g_matchPort = 0;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.WS] listeners stopped, %zu remote connection(s) closed — sockets released",
      remotes.size() + stoppedRouterRemotes);
}

// ============================================================================
// N61 behavioral test hooks — NEVR_TEST_HOOKS only.
//
// Expose the Close handler's callback-lifecycle decision to unit tests so the
// N61 regression can be verified: conn>=2 callback must survive conn=1 close.
// These manipulate file-static globals (g_pairs, g_loginRemoteWs, etc.) and
// are NEVER compiled into production builds.
// ============================================================================

#ifdef NEVR_TEST_HOOKS

std::string TestHook_BuildLoginRequest(uint64_t discordId, uint64_t platformCode,
                                       const std::string& displayName,
                                       const std::string& accessToken) {
  return BuildLoginRequest(discordId, platformCode, displayName, accessToken);
}

std::string TestHook_SelectRemoteBearer(bool hasUrlCredentials, const std::string& jwt,
                                        const std::string& serverKey) {
  return nevr_evr_codec::SelectRemoteBearer(hasUrlCredentials, jwt, serverKey);
}

bool TestHook_IsBearerReplacingPath(const std::string& url) { return nevr_evr_codec::IsBearerReplacingPath(url); }

void TestHook_SetGameFlagWord(int64_t flags) { g_testGameFlagWord = flags; }

uint64_t TestHook_SelectPlatformCode(bool hasUrlCredentials, bool noVr) {
  return nevr_evr_codec::SelectPlatformCode(hasUrlCredentials, noVr);
}

const char* TestHook_PlatformPrefix(uint64_t platformCode) {
  return nevr_evr_codec::PlatformPrefix(platformCode);
}

int TestHook_GuardWsCallbackForwardsArguments(int first, int second) {
  int observed = 0;
  const auto guarded = GuardWsCallback("ws_bridge_test_success",
                                       [&observed](int lhs, int rhs) {
                                         observed = lhs + rhs;
                                       });
  guarded(first, second);
  return observed;
}

bool TestHook_GuardWsCallbackContainsStdException() {
  const auto guarded = GuardWsCallback("ws_bridge_test", []() {
    throw std::runtime_error("response-secret-sentinel");
  });
  guarded();
  return true;
}

int TestHook_LogGameToServerFrame(const std::string& frame) {
  return LogGameToServerFrame(frame, "test-conn");
}

int TestHook_LogFrameMessages(const char* direction, int connIdx, const std::string& frame) {
  return LogFrameMessages(direction, connIdx, frame);
}

void TestHook_ObserveSocialFrames(const char* direction, int connIdx, const std::string& frame) {
  ObserveSocialFrames(direction, connIdx, frame);
}

void TestHook_ResetLobbyFrameLimiter() {
  std::lock_guard<std::mutex> lock(g_lobbyLimiterMutex);
  g_lobbyRecords.Reset();
}

void TestHook_SetLobbyFrameClock(uint64_t (*clockMs)()) {
  std::lock_guard<std::mutex> lock(g_lobbyLimiterMutex);
  g_lobbyClockMs = clockMs != nullptr ? clockMs : &RealLobbyClockMs;
}

void TestHook_SetLobbyPartySource(nevr_social_party::View (*view)()) {
  std::lock_guard<std::mutex> lock(g_lobbyLimiterMutex);
  g_lobbyPartyView = view != nullptr ? view : &RealLobbyPartyView;
}

bool TestHook_ReadLoginFailureDiagnostic(const std::string& frame, uint64_t* statusCode, size_t* messageBytes) {
  if (statusCode == nullptr || messageBytes == nullptr) return false;
  const std::optional<nevr_evr_codec::LoginFailure> diagnostic = nevr_evr_codec::ParseLoginFailure(frame);
  if (!diagnostic.has_value()) return false;
  *statusCode = diagnostic->statusCode;
  *messageBytes = diagnostic->messageBytes;
  return true;
}

bool TestHook_MoveCodeLineFirst(const std::string& frame, std::string* out) {
  if (out == nullptr) return false;
  const std::optional<std::string> reordered = MoveCodeLineFirst(frame);
  if (!reordered.has_value()) return false;
  *out = *reordered;
  return true;
}

bool TestHook_LogLoginFailureDiagnostic(const std::string& frame, bool serverMode) {
  const std::optional<nevr_evr_codec::LoginFailure> diagnostic = nevr_evr_codec::ParseLoginFailure(frame);
  const std::string message = nevr_log_diagnostics::FormatLoginFailureDiagnostic(
      diagnostic.has_value(), diagnostic ? diagnostic->statusCode : 0, diagnostic ? diagnostic->messageBytes : 0,
      serverMode);
  Log(EchoVR::LogLevel::Warning, "%s", message.c_str());
  return diagnostic.has_value();
}

bool TestHook_GuardWsCallbackPropagatesNonStdException() {
  const auto guarded = GuardWsCallback("ws_bridge_test_nonstd", []() {
    throw 7;
  });
  try {
    guarded();
  } catch (int value) {
    return value == 7;
  }
  return false;
}

// Create a real ix::WebSocket for use as a test handle. The test owns the
// returned shared_ptr and must keep it alive for the duration of the test.
// The WebSocket is never connected — it serves only as a map-key / callback
// target.
void* TestHook_N61_CreateMockWs() {
  auto* ws = new std::shared_ptr<ix::WebSocket>(
      std::make_shared<ix::WebSocket>());
  return static_cast<void*>(ws);
}

void TestHook_N61_DestroyMockWs(void* handle) {
  delete static_cast<std::shared_ptr<ix::WebSocket>*>(handle);
}

void* TestHook_N61_GetRawWsPtr(void* handle) {
  return static_cast<void*>(
      static_cast<std::shared_ptr<ix::WebSocket>*>(handle)->get());
}

// Register a simulated conn=1 (login) pair on a new remote.
// - remoteHandle: a mock WS returned by TestHook_N61_CreateMockWs (the remote)
// - gameWsHandle: a mock WS for the game-side login connection
// Returns: the login gameWs raw pointer (for later close simulation).
void* TestHook_N61_RegisterLogin(void* remoteHandle, void* gameWsHandle) {
  auto* remotePtr = static_cast<std::shared_ptr<ix::WebSocket>*>(remoteHandle);
  auto* gameWsPtr = static_cast<std::shared_ptr<ix::WebSocket>*>(gameWsHandle);
  ix::WebSocket* rawGameWs = gameWsPtr->get();

  auto pair = std::make_unique<ProxyPair>();
  pair->remoteWs = *remotePtr;
  pair->remoteOpen = true;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    pair->connIdx = g_roles.Admit(nevr_socket_role::Listener::Service, nevr_socket_role::Role::Login).connIdx;
  }
  pair->role = nevr_socket_role::Role::Login;

  // Login callback — captures a dummy that the test can later check.
  g_loginRemoteWs = *remotePtr;
  g_loginGameWs = rawGameWs;
  g_activeGameWs = rawGameWs;

  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    g_pairs[rawGameWs] = std::move(pair);
  }

  return static_cast<void*>(rawGameWs);
}

// Register a simulated conn>=2 (matchmaker) pair sharing the login remote.
// N61: registers its OWN callback on the shared remote.
void* TestHook_N61_RegisterMatchmaker(void* gameWsHandle, bool* callbackFired) {
  auto* gameWsPtr = static_cast<std::shared_ptr<ix::WebSocket>*>(gameWsHandle);
  ix::WebSocket* rawGameWs = gameWsPtr->get();

  auto pair = std::make_unique<ProxyPair>();
  pair->remoteWs = g_loginRemoteWs;
  pair->remoteOpen = true;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    pair->connIdx = 2 + static_cast<int>(g_pairs.size()) - 1;  // after the login pair: 2, 3, ...
  }
  pair->role = nevr_socket_role::Role::Matchmaker;

  // N61: matchmaker registers its own callback on the shared remote.
  bool* fired = callbackFired;
  g_loginRemoteWs->setOnMessageCallback(GuardWsCallback("ws_bridge.cpp:setOnMessageCallback", 
      [fired](const ix::WebSocketMessagePtr&) {
        if (fired) *fired = true;
      }));

  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    g_pairs[rawGameWs] = std::move(pair);
    g_activeGameWs = rawGameWs;
  }

  return static_cast<void*>(rawGameWs);
}

// Run the production Close handler for the given game WS and report whether
// the shared remote's callback was cleared (replaced with a no-op — N85:
// never nullptr, which ixwebsocket would invoke and throw bad_function_call).
// Returns true if the callback WAS cleared, false if it survived.
//
// This is NOT a reimplementation — it runs the SAME code as the production
// Close handler (same file, same static globals, same guard conditions).
// The test hook is the observer; the logic under test is production.
bool TestHook_ForgetLoginSession(void* remoteHandle, int* nextConnIdx) {
  auto* remotePtr = static_cast<std::shared_ptr<ix::WebSocket>*>(remoteHandle);
  std::shared_ptr<ix::WebSocket> ended;
  bool forgot = false;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    forgot = ForgetLoginSessionLocked(remotePtr->get(), &ended);
    if (nextConnIdx != nullptr) {
      // The slot the game's next /login socket would take, asked of a copy so nothing is claimed.
      nevr_socket_role::RoleTable next = g_roles;
      *nextConnIdx = next.Admit(nevr_socket_role::Listener::Service, nevr_socket_role::Role::Login).connIdx;
    }
  }
  ReleaseOffThread(std::move(ended));
  return forgot;
}

bool TestHook_SendFrameToServer(const std::string& frame) { return SendFrameToServer(frame); }

// Runs the production remote-ended path (the one the remote's Close and Error callbacks call).
void TestHook_CloseGameSocketsForRemote(void* remoteHandle, int connIdx, unsigned int code) {
  auto* remotePtr = static_cast<std::shared_ptr<ix::WebSocket>*>(remoteHandle);
  CloseGameSocketsForRemote(remotePtr->get(), connIdx, code);
}

namespace {

// A real ix::WebSocket reaching a real local ix server through a TCP forwarder that can end both of its connections
// with no close frame, so the remote sees the connection vanish with its state still OPEN.
class DropRig {
 public:
  using RemoteCallback = std::function<void(const ix::WebSocketMessagePtr&)>;

  // serverGreets: the server sends the client one message as soon as it opens.
  bool Start(bool serverGreets, RemoteCallback onRemote) {
    ix::initNetSystem();
    std::mt19937 rng{std::random_device{}()};
    for (int attempt = 0; attempt < 50 && !server_; ++attempt) {
      serverPort_ = 20000 + static_cast<int>(rng() % 30000);
      auto candidate = std::make_unique<ix::WebSocketServer>(serverPort_, "127.0.0.1");
      if (candidate->listen().first) server_ = std::move(candidate);
    }
    if (!server_) return false;
    opened_ = serverSawOpen_.get_future();
    server_->setOnClientMessageCallback([this, serverGreets](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& ws,
                                                             const ix::WebSocketMessagePtr& msg) {
      if (msg->type != ix::WebSocketMessageType::Open) return;
      if (serverGreets) ws.sendBinary("greeting");
      if (!openSignalled_.exchange(true)) serverSawOpen_.set_value();
    });
    server_->start();

    front_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int addrLen = sizeof(addr);
    if (front_ == INVALID_SOCKET || bind(front_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(front_, 1) != 0 || getsockname(front_, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
      if (front_ != INVALID_SOCKET) closesocket(front_);
      front_ = INVALID_SOCKET;
      server_->stop();
      return false;
    }
    const int frontPort = ntohs(addr.sin_port);
    acceptor_ = std::thread([this] { Accept(); });

    clientOpen_ = clientSawOpen_.get_future();
    remote = std::make_shared<ix::WebSocket>();
    remote->setUrl("ws://127.0.0.1:" + std::to_string(frontPort) + "/");
    remote->disableAutomaticReconnection();
    remote->setOnMessageCallback([this, onRemote = std::move(onRemote)](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Open && !clientOpenSignalled_.exchange(true)) {
        clientSawOpen_.set_value();
      }
      onRemote(msg);
    });
    remote->start();
    return true;
  }

  // Both ends have seen the connection open (the drop must come after, or it is a failed connect).
  bool WaitOpen(std::chrono::milliseconds limit) {
    return opened_.wait_for(limit) == std::future_status::ready &&
           clientOpen_.wait_for(limit) == std::future_status::ready;
  }

  // reset: the forwarder's sockets are closed with a reset, so the remote's next send fails (a shutdown leaves the
  // remote able to write into the kernel's buffer).
  void Drop(bool reset = false) {
    for (std::atomic<SOCKET>* side : {&clientSide_, &serverSide_}) {
      const SOCKET s = side->load();
      if (s == INVALID_SOCKET) continue;
      if (reset) {
        linger abortive{1, 0};
        setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&abortive), sizeof(abortive));
        side->store(INVALID_SOCKET);
        closesocket(s);
      } else {
        shutdown(s, SD_BOTH);
      }
    }
  }

  // Ends everything. Only when no thread of the rig is stuck.
  void Teardown() {
    remote->stop();
    if (front_ != INVALID_SOCKET) closesocket(front_);  // wakes the acceptor if the client never connected
    acceptor_.join();
    for (std::thread& t : pumps_) t.join();
    for (SOCKET s : {clientSide_.load(), serverSide_.load()}) {
      if (s != INVALID_SOCKET) closesocket(s);
    }
    server_->stop();
  }

  // A thread may be stuck and the helper threads still use this frame: leave them running and detached for the
  // caller to end the process.
  void Detach() {
    acceptor_.detach();
    std::lock_guard<std::mutex> lk(pumpsMutex_);
    for (std::thread& t : pumps_) t.detach();
  }

  std::shared_ptr<ix::WebSocket> remote;

 private:
  void Accept() {
    SOCKET a = accept(front_, nullptr, nullptr);
    if (a == INVALID_SOCKET) return;
    SOCKET b = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(static_cast<u_short>(serverPort_));
    if (b == INVALID_SOCKET || connect(b, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0) {
      closesocket(a);
      if (b != INVALID_SOCKET) closesocket(b);
      return;
    }
    clientSide_ = a;
    serverSide_ = b;
    auto pump = [](SOCKET from, SOCKET dest) {
      char buf[4096];
      for (;;) {
        const int n = recv(from, buf, sizeof(buf), 0);
        if (n <= 0) return;
        if (send(dest, buf, n, 0) != n) return;
      }
    };
    std::lock_guard<std::mutex> lk(pumpsMutex_);
    pumps_.emplace_back(pump, a, b);
    pumps_.emplace_back(pump, b, a);
  }

  std::unique_ptr<ix::WebSocketServer> server_;
  int serverPort_ = 0;
  SOCKET front_ = INVALID_SOCKET;
  std::atomic<SOCKET> clientSide_{INVALID_SOCKET};
  std::atomic<SOCKET> serverSide_{INVALID_SOCKET};
  std::vector<std::thread> pumps_;
  std::mutex pumpsMutex_;
  std::thread acceptor_;
  std::promise<void> serverSawOpen_;
  std::future<void> opened_;
  std::atomic<bool> openSignalled_{false};
  std::promise<void> clientSawOpen_;
  std::future<void> clientOpen_;
  std::atomic<bool> clientOpenSignalled_{false};
};

}  // namespace

// The login remote dropped by its server: ix calls the Close callback on the remote's own thread under its
// non-recursive _setReadyStateMutex, and a send on that remote fails in sendOnSocket and takes that mutex again
// (#723). True when the callback's CloseGameSocketsForRemote returned in time; false leaves that thread stuck, so the
// caller must end the process rather than tear down.
bool TestHook_LoginRemoteDroppedThroughRealIx(unsigned int timeoutMs) {
  DropRig rig;
  std::promise<void> returned;
  std::future<void> finished = returned.get_future();
  std::atomic<bool> fired{false};
  DropRig* rigPtr = &rig;  // the callback reads the remote's address when it runs, after Start has set it
  if (!rig.Start(false, [&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type != ix::WebSocketMessageType::Close || fired.exchange(true)) return;
        CloseGameSocketsForRemote(rigPtr->remote.get(), 1, msg->closeInfo.code);
        returned.set_value();
      })) {
    return false;
  }
  void* remoteHandle = new std::shared_ptr<ix::WebSocket>(rig.remote);
  void* gameHandle = TestHook_N61_CreateMockWs();
  TestHook_N61_RegisterLogin(remoteHandle, gameHandle);

  const auto deadline = std::chrono::milliseconds(timeoutMs);
  bool ok = rig.WaitOpen(deadline);
  if (!ok) std::fprintf(stderr, "TestHook_LoginRemoteDroppedThroughRealIx: the remote never opened\n");
  if (ok) {
    rig.Drop();  // the connection ends with no close frame
    ok = finished.wait_for(deadline) == std::future_status::ready;
    if (!ok) std::fprintf(stderr, "TestHook_LoginRemoteDroppedThroughRealIx: the Close callback never returned\n");
  }
  if (!ok) {
    rig.Detach();
    return false;
  }
  rig.Teardown();
  TestHook_N61_DestroyMockWs(gameHandle);
  delete static_cast<std::shared_ptr<ix::WebSocket>*>(remoteHandle);
  return true;
}

// #738: the bridge sends on the login remote from the caller's own thread. When the connection has gone, that send
// fails inside ix and ix runs the remote's Close callback on the sending thread; the callback takes g_pairsMutex, so
// a sender that holds g_pairsMutex deadlocks on itself. The remote's reader is parked inside a Message callback so
// the failure is found by the send and not by the reader. `path` names the production send under test: 0 is
// SendFrameToServer, 1 the game->server forward, 2 the flush when the remote opens. 0: the send failed and its
// Close callback returned; 1: the sending thread never came back (end the process); 2: no send ever failed (the rig
// is broken, not the bridge).
int TestHook_SendOnDroppedRemote(int path, unsigned int timeoutMs) {
  DropRig rig;
  std::promise<void> parked;
  std::future<void> parkedF = parked.get_future();
  std::promise<void> release;
  std::shared_future<void> releaseF = release.get_future().share();
  std::atomic<bool> parkedSignalled{false};
  std::promise<void> closed;
  std::future<void> closedF = closed.get_future();
  std::atomic<bool> closeFired{false};
  std::atomic<bool> closeEntered{false};  // ix has run the Close callback (on the sender or the reader)
  DropRig* rigPtr = &rig;
  if (!rig.Start(true, [&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
          if (!parkedSignalled.exchange(true)) parked.set_value();
          releaseF.wait();  // the reader stays here: it cannot be the one to notice the drop
        } else if (msg->type == ix::WebSocketMessageType::Close && !closeFired.exchange(true)) {
          closeEntered = true;
          CloseGameSocketsForRemote(rigPtr->remote.get(), 1, msg->closeInfo.code);
          closed.set_value();
        }
      })) {
    return 2;
  }
  void* remoteHandle = new std::shared_ptr<ix::WebSocket>(rig.remote);
  void* gameHandle = TestHook_N61_CreateMockWs();
  ix::WebSocket* rawGame = static_cast<ix::WebSocket*>(TestHook_N61_RegisterLogin(remoteHandle, gameHandle));
  const auto deadline = std::chrono::milliseconds(timeoutMs);
  const bool ready = rig.WaitOpen(deadline) && parkedF.wait_for(deadline) == std::future_status::ready;
  int result = 2;
  if (ready) {
    rig.Drop(true);
    std::promise<void> workerDone;
    std::future<void> workerDoneF = workerDone.get_future();
    std::thread worker([&] {
      const std::string frame = "frame";
      while (closedF.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
        if (path == 0) {
          SendFrameToServer(frame);
        } else if (path == 1) {
          ForwardGameFrameToRemote(*rawGame, frame, true);
        } else {
          ProxyPair* pair = nullptr;
          {
            std::lock_guard<std::mutex> lk(g_pairsMutex);
            pair = g_pairs.at(rawGame).get();
            pair->remoteOpen = false;
            pair->loginInjected = true;  // the flush under test, not the login request
            pair->pendingToRemote.assign(1, frame);
          }
          HandleRemoteOpen(pair, 1, 0, "", "");
        }
      }
      workerDone.set_value();
    });
    if (workerDoneF.wait_for(deadline) == std::future_status::ready) {
      worker.join();
      result = 0;
    } else {
      // Stuck with the Close callback entered: the sender is waiting on g_pairsMutex inside it. Stuck without: no send
      // ever failed and no Close ran, which is the rig's fault.
      result = closeEntered.load() ? 1 : 2;
      std::fprintf(stderr, "TestHook_SendOnDroppedRemote(path=%d): the sending thread never returned (close_entered=%d)\n",
                   path, closeEntered.load() ? 1 : 0);
      worker.detach();
    }
  }
  if (result != 0 && ready) {
    rig.Detach();
    return result;
  }
  release.set_value();
  rig.Teardown();
  TestHook_N61_DestroyMockWs(gameHandle);
  delete static_cast<std::shared_ptr<ix::WebSocket>*>(remoteHandle);
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    g_pairs.erase(rawGame);
  }
  return result;
}

size_t TestHook_GameSocketsBoundTo(void* remoteHandle) {
  auto* remotePtr = static_cast<std::shared_ptr<ix::WebSocket>*>(remoteHandle);
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  return GameSocketsBoundToLocked(remotePtr->get()).size();
}

bool TestHook_N61_SimulateCloseAndCheckCleared(void* rawGameWsPtr) {
  ix::WebSocket* gameWs = static_cast<ix::WebSocket*>(rawGameWsPtr);

  // Run the production Close handler. Track whether the guard cleared
  // the callback (the condition under test for N61).
  bool callbackWasCleared = false;
  {
    std::shared_ptr<ix::WebSocket> remoteToStop;
    {
      std::lock_guard<std::mutex> lk(g_pairsMutex);
      int closedConnIdx = -1;
      remoteToStop = RetireGameWsLocked(gameWs, &closedConnIdx, &callbackWasCleared);
    }
    if (remoteToStop) {
      remoteToStop->stop();
    }
  }

  return callbackWasCleared;
}

// Check whether the shared remote has an active callback by setting a
// temporary one and checking if it replaces successfully. Returns true
// if a callback is active (the test callback replaced something).
int TestHook_SharedRouteConn() {
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  ix::WebSocket* target = nullptr;
  const ProxyPair* route = SharedRouteLocked(&target);
  return route != nullptr ? route->connIdx : -1;
}

bool TestHook_N61_HasActiveCallback() {
  if (!g_loginRemoteWs) return false;
  // We can't directly query ix::WebSocket's internal callback state.
  // Workaround: the test tracks this via the return value of
  // SimulateCloseAndCheckCleared + the matchmaker's callbackFired flag.
  return g_loginRemoteWs != nullptr;
}

// Check whether g_pairsMutex is currently free (not held by any thread).
// WOULD-FAIL-IF: delete the `} // g_pairsMutex RELEASED` line at the Close handler —
// without releasing the mutex before stop(), try_lock fails after Close returns.
#ifdef NEVR_TEST_HOOKS
bool TestHook_N60_IsMutexFree() {
    bool free = g_pairsMutex.try_lock();
    if (free) g_pairsMutex.unlock();
    return free;
}
#endif

size_t TestHook_ConnectedGameSockets() {
  size_t connected = 0;
  if (g_server) connected += g_server->getClients().size();
  if (g_matchServer) connected += g_matchServer->getClients().size();
  return connected;
}

void* TestHook_LoginGameWsPtr() {
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  return static_cast<void*>(g_loginGameWs);
}

void TestHook_SharedRoute_SetBeforeSend(void (*fn)(void* target)) {
  g_testBeforeSharedSend = reinterpret_cast<void (*)(ix::WebSocket*)>(fn);
}

// A weak handle on the first game-side socket the service listener holds; empty when there is none.
std::weak_ptr<void> TestHook_FirstServiceClientWeak() {
  if (!g_server) return {};
  for (const auto& c : g_server->getClients()) return std::weak_ptr<void>(c);
  return {};
}

// Reset all internal bridge state for the next test.
void TestHook_SetRouterGate(int gate) { g_testRouterGate = gate; }
void TestHook_SetRefuseRemoteOpens(bool refuse) { g_testRefuseRemoteOpens = refuse; }
void TestHook_ReevaluateRouterGate() {
  if (RouterPath* routed = g_routerPath.load(); routed != nullptr) routed->router.ReevaluateHeldLogins();
}
bool TestHook_RouterStats(uint64_t* droppedGame, uint64_t* droppedRemote, uint64_t* droppedUnrequires,
                          uint64_t* loginsRemoved, uint64_t* heldRemotes) {
  RouterPath* routed = g_routerPath.load();
  if (routed == nullptr) return false;
  const nevr_session_router::Stats stats = routed->router.GetStats();
  if (droppedGame != nullptr) *droppedGame = stats.droppedGameFrames;
  if (droppedRemote != nullptr) *droppedRemote = stats.droppedRemoteFrames;
  if (droppedUnrequires != nullptr) *droppedUnrequires = stats.droppedUnrequires;
  if (loginsRemoved != nullptr) *loginsRemoved = stats.loginsRemoved;
  if (heldRemotes != nullptr) *heldRemotes = stats.heldRemotes;
  return true;
}

void TestHook_N61_ResetState() {
  g_testRouterGate = -1;
  g_testRefuseRemoteOpens = false;
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  g_pairs.clear();
  g_loginRemoteWs.reset();
  g_loginGameWs = nullptr;
  g_activeGameWs = nullptr;
  g_roles.Reset();
  nevr_socket_role::GlobalAdvertisement().Reset();
  std::lock_guard<std::mutex> refusalLock(g_refusalMutex);
  g_refusalLatch = nevr_login_refusal::ClientRefusalLatch();
  g_localAnswers = nevr_login_refusal::LocalAnswerCounter();
  g_routerRefusalKeys.clear();
}

// ---- test rig for the flush in HandleRemoteOpen ----------------------------------------------------------------
// A real ix server that records every frame it receives, in order, and a real ix client (R1) connected to it. A
// login pair on R1 is registered with remoteOpen=false and frames queued, the way the production Open handler finds
// it. The test parks HandleRemoteOpen between its first batch of sends and its re-lock (through the test binary's
// Log, on the "login injected" line) and drives the other paths while it is parked.
namespace {
struct FlushRig {
  std::unique_ptr<ix::WebSocketServer> server;
  std::mutex m;
  std::condition_variable cv;
  std::vector<std::string> got;
  std::shared_ptr<ix::WebSocket> r1;
  std::promise<void> r1Open;
  std::atomic<bool> r1OpenSignalled{false};
  void* game1 = nullptr;  // TestHook_N61_CreateMockWs handles (only their addresses are used)
  void* game2 = nullptr;
  void* game3 = nullptr;
  std::shared_ptr<ix::WebSocket> r2;  // the replacement pair's own remote: never started
  ProxyPair* pair = nullptr;          // the login pair HandleRemoteOpen is given
  ProxyPair* replacement = nullptr;   // the pair registered under game2 at the same address
};
}  // namespace

void* TestHook_FlushRig_Start(unsigned int timeoutMs) {
  ix::initNetSystem();
  auto* rig = new FlushRig();
  std::mt19937 rng{std::random_device{}()};
  int port = 0;
  for (int attempt = 0; attempt < 50 && !rig->server; ++attempt) {
    port = 20000 + static_cast<int>(rng() % 30000);
    auto candidate = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1");
    if (candidate->listen().first) rig->server = std::move(candidate);
  }
  if (!rig->server) { delete rig; return nullptr; }
  rig->server->setOnClientMessageCallback([rig](std::shared_ptr<ix::ConnectionState>, ix::WebSocket&,
                                                const ix::WebSocketMessagePtr& msg) {
    if (msg->type != ix::WebSocketMessageType::Message) return;
    std::lock_guard<std::mutex> lk(rig->m);
    rig->got.push_back(msg->str);
    rig->cv.notify_all();
  });
  rig->server->start();
  rig->r1 = std::make_shared<ix::WebSocket>();
  rig->r1->setUrl("ws://127.0.0.1:" + std::to_string(port) + "/");
  rig->r1->disableAutomaticReconnection();
  rig->r1->setOnMessageCallback([rig](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open && !rig->r1OpenSignalled.exchange(true)) rig->r1Open.set_value();
  });
  std::future<void> opened = rig->r1Open.get_future();
  rig->r1->start();
  if (opened.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) return nullptr;

  rig->game1 = TestHook_N61_CreateMockWs();
  rig->game2 = TestHook_N61_CreateMockWs();
  rig->game3 = TestHook_N61_CreateMockWs();
  rig->r2 = std::make_shared<ix::WebSocket>();
  auto pair = std::make_unique<ProxyPair>();
  pair->remoteWs = rig->r1;
  pair->remoteOpen = false;
  pair->loginInjected = false;
  pair->connIdx = 1;
  pair->role = nevr_socket_role::Role::Login;
  pair->pendingToRemote = {"q1", "q2"};
  rig->pair = pair.get();
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  ix::WebSocket* g1 = static_cast<std::shared_ptr<ix::WebSocket>*>(rig->game1)->get();
  g_pairs[g1] = std::move(pair);
  g_loginRemoteWs = rig->r1;
  g_loginGameWs = g1;
  g_activeGameWs = g1;
  return rig;
}

// The production Open handler for the login pair, on the calling thread.
void TestHook_FlushRig_RunOpen(void* handle) {
  auto* rig = static_cast<FlushRig*>(handle);
  HandleRemoteOpen(rig->pair, 1, 4242, "verify748", "");
}

// The production game->server forward for game socket 1 (the login socket) or 3 (a matchmaker sharing R1).
void TestHook_FlushRig_Forward(void* handle, int game, const std::string& frame) {
  auto* rig = static_cast<FlushRig*>(handle);
  void* h = game == 3 ? rig->game3 : rig->game1;
  ForwardGameFrameToRemote(*static_cast<std::shared_ptr<ix::WebSocket>*>(h)->get(), frame, true);
}

// A matchmaker pair sharing R1, registered as the production Open handler does (remoteOpen=true at admission).
void TestHook_FlushRig_AddMatchmaker(void* handle) {
  auto* rig = static_cast<FlushRig*>(handle);
  auto pair = std::make_unique<ProxyPair>();
  pair->remoteWs = rig->r1;
  pair->remoteOpen = true;
  pair->loginInjected = true;
  pair->connIdx = 2;
  pair->role = nevr_socket_role::Role::Matchmaker;
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  g_pairs[static_cast<std::shared_ptr<ix::WebSocket>*>(rig->game3)->get()] = std::move(pair);
}

// The login game socket closes (its pair is erased, as RetireGameWsLocked does) and a new game socket's pair is
// registered under game2 at the address the erased pair had. allocatorReused: the allocator handed the address
// back (false: not reproduced on this run, nothing registered).
bool TestHook_FlushRig_ReplacePairAtSameAddress(void* handle, bool* allocatorReused) {
  auto* rig = static_cast<FlushRig*>(handle);
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  ix::WebSocket* g1 = static_cast<std::shared_ptr<ix::WebSocket>*>(rig->game1)->get();
  ix::WebSocket* g2 = static_cast<std::shared_ptr<ix::WebSocket>*>(rig->game2)->get();
  auto it = g_pairs.find(g1);
  if (it == g_pairs.end()) return false;
  ProxyPair* const old = it->second.get();
  g_pairs.erase(it);  // frees the old pair
  // The allocator only: up to 1000 live allocations of the same type until one lands on the freed address.
  std::vector<std::unique_ptr<ProxyPair>> misses;
  std::unique_ptr<ProxyPair> fresh = std::make_unique<ProxyPair>();
  while (fresh.get() != old && misses.size() < 1000) {
    misses.push_back(std::move(fresh));
    fresh = std::make_unique<ProxyPair>();
  }
  *allocatorReused = fresh.get() == old;
  std::fprintf(stderr, "flush rig: old pair %p, new pair %p after %zu misses\n", static_cast<void*>(old),
               static_cast<void*>(fresh.get()), misses.size());
  if (!*allocatorReused) return false;
  fresh->remoteWs = rig->r2;  // its own remote, still connecting
  fresh->remoteOpen = false;
  fresh->loginInjected = false;
  fresh->connIdx = 1;
  fresh->role = nevr_socket_role::Role::Login;
  fresh->pendingToRemote = {"x-for-r2"};
  rig->replacement = fresh.get();
  g_pairs[g2] = std::move(fresh);
  g_loginGameWs = g2;
  g_activeGameWs = g2;
  return true;
}

void TestHook_FlushRig_ReplacementState(void* handle, bool* remoteOpen, size_t* pending) {
  auto* rig = static_cast<FlushRig*>(handle);
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  *remoteOpen = rig->replacement->remoteOpen;
  *pending = rig->replacement->pendingToRemote.size();
}

bool TestHook_FlushRig_LoginPairOpen(void* handle) {
  auto* rig = static_cast<FlushRig*>(handle);
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  return PairRegisteredLocked(rig->pair) && rig->pair->remoteOpen;
}

// Frames the server received, waiting until there are at least `n` or the time runs out.
std::vector<std::string> TestHook_FlushRig_Received(void* handle, size_t n, unsigned int timeoutMs) {
  auto* rig = static_cast<FlushRig*>(handle);
  std::unique_lock<std::mutex> lk(rig->m);
  rig->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return rig->got.size() >= n; });
  return rig->got;
}

void TestHook_FlushRig_Stop(void* handle) {
  auto* rig = static_cast<FlushRig*>(handle);
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    g_pairs.clear();
    g_loginRemoteWs.reset();
    g_loginGameWs = nullptr;
    g_activeGameWs = nullptr;
  }
  rig->r1->stop();
  rig->server->stop();
  TestHook_N61_DestroyMockWs(rig->game1);
  TestHook_N61_DestroyMockWs(rig->game2);
  TestHook_N61_DestroyMockWs(rig->game3);
  delete rig;
}

#endif  // NEVR_TEST_HOOKS
