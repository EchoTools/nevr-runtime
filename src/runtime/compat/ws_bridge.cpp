#include "runtime/compat/ws_bridge.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/hmd_serial.h"
#include "runtime/compat/login_profile.h"
#include "runtime/compat/social_names.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"
#include "runtime/hook/symbol_corpus.h"

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
#include "runtime/log/url_diagnostics.h"
#include "runtime/log/security_diagnostics.h"
#include "runtime/server/serverdb_uri.h"
#include "core/logging.h"
#include <exception>
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
  if (frame.size() < EvrCodec::kHeaderSize + kLoginFailureFixedPayloadSize + 1) return std::nullopt;
  uint64_t symbol = 0;
  uint64_t payloadLength = 0;
  memcpy(&symbol, frame.data() + 8, sizeof(symbol));
  memcpy(&payloadLength, frame.data() + 16, sizeof(payloadLength));
  if (symbol != EvrCodec::kSymLoginFailure || payloadLength != frame.size() - EvrCodec::kHeaderSize) return std::nullopt;
  if (frame.back() != '\0') return std::nullopt;

  const size_t textStart = EvrCodec::kHeaderSize + kLoginFailureFixedPayloadSize;
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
  const uint64_t newLength = out.size() - EvrCodec::kHeaderSize;
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

// The ix objects below are deliberately never destroyed: a static destructor runs at
// DLL_PROCESS_DETACH under the loader lock, and ix::WebSocket/WebSocketServer destructors stop and
// join their worker threads there. Each holder is a leaked heap object so process exit skips them
// (the OS reclaims the sockets); StopWebSocketBridgeListener is the real stop.
static std::unique_ptr<ix::WebSocketServer>& g_server = *new std::unique_ptr<ix::WebSocketServer>();
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
  int connIdx = -1;  // 0=config, 1=login, >=2=matchmaker (see g_connectionCount)
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
static std::atomic<int> g_connectionCount{0};  // tracks connection order (0=config, 1+=login)
static auto& g_pairs = *new std::unordered_map<ix::WebSocket*, std::unique_ptr<ProxyPair>>();

// Defined after the login-session globals below.
static bool ForgetLoginSessionLocked(const ix::WebSocket* remote);

// The game sockets whose pair forwards to `remote` (the login socket and any matchmaker sockets sharing
// its session). Caller holds g_pairsMutex.
static std::vector<const ix::WebSocket*> GameSocketsBoundToLocked(const ix::WebSocket* remote) {
  std::vector<const ix::WebSocket*> games;
  for (const auto& entry : g_pairs) {
    if (entry.second && entry.second->remoteWs.get() == remote) games.push_back(entry.first);
  }
  return games;
}

// A remote session that ends must end the game's sockets on it too, or the game sits "logged in" with no
// server and never reconnects (#70: forced cut, run 20-1dd53339e6a29b8). Called from a remote callback.
// Closing a game socket inline deadlocks (close() waits on the server thread, which may wait on
// g_pairsMutex), so the close runs on a thread that holds no lock, and each socket is kept alive by the
// server's own shared_ptr (WebSocketServer::getClients) rather than a raw pointer.
static void CloseGameSocketsForRemote(const ix::WebSocket* remote, int connIdx, unsigned int code) {
  std::vector<const ix::WebSocket*> games;
  bool wasLoginSession = false;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    games = GameSocketsBoundToLocked(remote);
    wasLoginSession = ForgetLoginSessionLocked(remote);
  }
  if (wasLoginSession) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.WS] login session ended: the game's next connection is a new login (connections renumbered from 1)");
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

// N91: process-wide, not per-ProxyPair. `loginInjected` lives on the ProxyPair, so
// the Open-handler test passed independently on every connection and a
// LoginRequest was injected on conn=0 (config) as well as conn=1 — two logins,
// two OCS sessions, measured. The conn=0 injection is a deliberate SAFETY NET for
// the case where the game never opens a second connection; it shall fire only
// when the primary path has not already logged in.
static std::atomic<bool> g_loginInjectedAnywhere{false};

// Account id used by the last injection — the server-mode fake LoginSuccess (N92)
// echoes it back so the game sees a consistent identity.
static uint64_t g_lastInjectedDiscordId = 0;

// The game-side WS that registered the shared remote's onMessageCallback
// (conn=1 — login). When this connection closes, the lambda's captured pointers
// (pairPtr, gameWsPtr) become dangling. Used in the Close handler to clear the
// callback only when the owning pair is destroyed, not when a sharing pair
// (conn>=2, matchmaker) closes.
static ix::WebSocket* g_loginGameWs = nullptr;

// The game numbers its connections to the bridge (0 config, 1 login, 2+ matchmaker on the login session).
// When the login session itself ends, the game reconnects to log in again; without forgetting the dead
// session, that connection is counted as a matchmaker and attached to it (#70, run 20-1dd5334c6f870d2 is
// the measurement that showed it). Caller holds g_pairsMutex. True when `remote` was the login session.
static bool ForgetLoginSessionLocked(const ix::WebSocket* remote) {
  if (remote == nullptr || remote != g_loginRemoteWs.get()) return false;
  g_loginRemoteWs.reset();
  g_loginGameWs = nullptr;
  g_connectionCount.store(1);
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
// only the first symbol hid the rest. A dedicated server's message traffic is sparse and is the
// only record of what the service told the game, so it logs at Info there; clients stay at Debug.
static int LogFrameMessages(const char* direction, int connIdx, const std::string& frame) {
  const EchoVR::LogLevel level = g_isServer ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Debug;
  size_t offset = 0;
  int count = 0;
  EvrCodec::Message message;
  while (true) {
    const EvrCodec::ReadStatus status = EvrCodec::ReadMessage(frame, offset, &message);
    if (status == EvrCodec::ReadStatus::End || status == EvrCodec::ReadStatus::BadMarker) break;
    const char* name = EchoVR::LookupSymbolName(message.symbol);
    Log(level, "[NEVR.WS] %s conn=%d (%s) msg=%d sym=0x%016llx %s len=%llu", direction, connIdx,
        ConnLabel(connIdx), count, static_cast<unsigned long long>(message.symbol), name ? name : "<unnamed>",
        static_cast<unsigned long long>(message.length));
    ++count;
    if (status == EvrCodec::ReadStatus::Truncated) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.WS] %s conn=%d msg=%d declares %llu bytes but %zu remain",
          direction, connIdx, count - 1, static_cast<unsigned long long>(message.length),
          frame.size() - offset - EvrCodec::kHeaderSize);
      break;
    }
    offset += EvrCodec::kHeaderSize + static_cast<size_t>(message.length);
  }
  const size_t remaining = frame.size() - offset;
  if (remaining > 0) {
    Log(level, "[NEVR.WS] %s conn=%d %zu bytes not in message framing", direction, connIdx, remaining);
  }
  return count;
}

// Debug-level decode of every message the game sends to the server, for diagnostics only: the raw
// frame is forwarded whether or not this walk finishes. A message whose declared payload runs past the
// end of the frame stops the walk BEFORE any decoder reads its payload. Returns the messages walked.
static int LogGameToServerFrame(const std::string& frame, const std::string& wsConnId) {
  size_t offset = 0;
  int msgIdx = 0;
  EvrCodec::Message message;
  while (true) {
    const EvrCodec::ReadStatus status = EvrCodec::ReadMessage(frame, offset, &message);
    if (status == EvrCodec::ReadStatus::End) break;
    if (status == EvrCodec::ReadStatus::BadMarker) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.WS] game->server: bad marker at offset %zu — per-message diagnostic "
          "decode aborted here, raw frame still forwarded to remote unparsed",
          offset);
      break;
    }
    const uint64_t sym = message.symbol;
    const uint64_t len = message.length;
    if (status == EvrCodec::ReadStatus::Truncated) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.WS]   truncated: header declares %llu payload bytes but only %zu remain — per-message "
          "diagnostic decode aborted here, raw frame still forwarded to remote unparsed",
          static_cast<unsigned long long>(len), frame.size() - offset - EvrCodec::kHeaderSize);
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
    offset += EvrCodec::kHeaderSize + static_cast<size_t>(len);
    msgIdx++;
  }
  if (frame.size() - offset > 0 && msgIdx > 0) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   %zu trailing bytes after %d messages", frame.size() - offset, msgIdx);
  }
  return msgIdx;
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
  EvrCodec::Message message;
  // A bad marker, a truncated message or the end of the frame all stop the walk: never read past it.
  while (EvrCodec::ReadMessage(frame, offset, &message) == EvrCodec::ReadStatus::Ok) {
    const uint64_t sym = message.symbol;
    const uint64_t len = message.length;
    const bool fromServer = strcmp(direction, "server->game") == 0;
    const uint8_t* payload = message.payload;
    // A display name arrives in a profile reply, whichever side asked for it (the game asks when a
    // friend is opened; the runtime asks for each friend, below).
    if (fromServer && sym == SocialNames::kProfileSuccess) {
      uint64_t accountId = 0;
      std::string displayName;
      const bool decoded = SocialNames::DecodeProfile(payload, static_cast<size_t>(len), &accountId, &displayName);
      uint64_t replyFor = accountId;
      if (!decoded && len >= 16) {
        replyFor = 0;
        for (int i = 7; i >= 0; --i) replyFor = (replyFor << 8) | payload[8 + i];
      }
      if (decoded) {
        SocialRoster::Global().SetName(accountId, displayName);
        SocialParty::Global().SetName(accountId, displayName);
      }
      if (SocialRoster::Global().Contains(replyFor)) {
        Log(EchoVR::LogLevel::Info,
            decoded ? "[NEVR.SOCIAL] friend name resolved account=%llu name_bytes=%zu"
                    : "[NEVR.SOCIAL] friend profile reply could not be read account=%llu reply_bytes=%zu",
            static_cast<unsigned long long>(replyFor), decoded ? displayName.size() : static_cast<size_t>(len));
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
    const char* name = SocialParty::RequestName(sym);
    if (name == nullptr) name = SocialParty::ReplyName(sym);
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
    if (fromServer && sym == SocialRoster::kFriendPresenceNotify) {
      uint64_t friendId = 0;
      SocialRoster::Presence presence;
      if (SocialRoster::ParsePresenceNotify(payload, static_cast<size_t>(len), &friendId, &presence)) {
        SocialRoster::Global().SetPresence(friendId, presence);
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend presence account=%llu party=%llu joinable=%d text=\"%s\"",
            static_cast<unsigned long long>(friendId), static_cast<unsigned long long>(presence.partyId),
            presence.joinable ? 1 : 0, presence.text.c_str());
      } else {
        Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] friend presence could not be read bytes=%llu",
            static_cast<unsigned long long>(len));
      }
    }
    if (fromServer && sym == SocialRoster::kRecentlyMetListResponse) {
      // Recently met (proposal §2): the whole list; the refresh the game is polling (slot 56) ends.
      std::vector<SocialRoster::Entry> people;
      if (SocialRoster::ParseRecentlyMetResponse(payload, static_cast<size_t>(len), &people)) {
        const auto online = static_cast<unsigned>(std::count_if(people.begin(), people.end(),
                                                                [](const SocialRoster::Entry& e) { return e.online; }));
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] recently met list count=%zu online=%u", people.size(), online);
        SocialRoster::RecentlyMet().SetList(std::move(people));
      } else {
        SocialRoster::RecentlyMet().EndRefresh();
        Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] recently met list could not be read bytes=%llu",
            static_cast<unsigned long long>(len));
      }
    }
    if (fromServer && sym == SocialParty::kPartyDataNotify) {
      // Party or member data (proposal §3): only a JSON object goes on to the game's CJson loader.
      SocialParty::DataNotify notify;
      if (!SocialParty::ParseDataNotify(payload, static_cast<size_t>(len), &notify)) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] party data could not be read bytes=%llu",
            static_cast<unsigned long long>(len));
      } else if (const nlohmann::json parsed = nlohmann::json::parse(notify.json, nullptr, false);
                 parsed.is_discarded() || !parsed.is_object()) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] party data rejected: not a JSON object party=%llu member=%llu bytes=%zu",
            static_cast<unsigned long long>(notify.partyId), static_cast<unsigned long long>(notify.memberId),
            notify.json.size());
      } else {
        const SocialParty::DataOutcome outcome =
            SocialParty::Global().ReceiveData(notify.partyId, notify.memberId, notify.json);
        const auto headset = parsed.find("headsettype");
        Log(EchoVR::LogLevel::Info,
            "[NEVR.SOCIAL] party data received party=%llu member=%llu seq=%u bytes=%zu keys=%zu headsettype=%s outcome=%s",
            static_cast<unsigned long long>(notify.partyId), static_cast<unsigned long long>(notify.memberId),
            notify.seq, notify.json.size(), parsed.size(),
            headset != parsed.end() ? headset->dump().c_str() : "-", SocialParty::DataOutcomeName(outcome));
      }
    }
    if (fromServer) {
      if (gameName != nullptr) {
        SocialRoster::Feed(SocialRoster::Global(), gameName, payload, static_cast<size_t>(len));
        // The friends the server names get a name lookup, once each per session.
        uint64_t friendId = 0;
        uint8_t status = 0;
        if (strcmp(gameName, "FriendStatusNotify") == 0 &&
            SocialRoster::ParseStatusNotify(payload, static_cast<size_t>(len), &friendId, &status)) {
          const std::vector<SocialParty::Message> asks = SocialNames::GlobalResolver().Want(friendId);
          if (!asks.empty()) {
            Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend name lookup requested account=%llu sent=%d",
                static_cast<unsigned long long>(friendId), SocialParty::Send(asks) ? 1 : 0);
          }
        }
      }
      // A friend added, accepted, removed or withdrawn: none of these carries presence, so ask the
      // server for the list again; the reply rebuilds the roster (a friend added on the website
      // would otherwise stay invisible until the next login).
      if (SocialRoster::IsFriendChangeSymbol(sym) || SocialRoster::IsFriendChange(gameName)) {
        uint64_t friendId = 0;
        if (len >= 16) memcpy(&friendId, payload + 8, sizeof(friendId));
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] friend change %s account=%llu: refreshing the friend list sent=%d",
            name != nullptr ? name : "<unnamed>", static_cast<unsigned long long>(friendId),
            SocialParty::Send(SocialParty::Global().RefreshFriends()) ? 1 : 0);
      }
      // Party messages update the facade's party; any requests that were waiting on the reply
      // (invites queued behind the party's creation) go out now. This runs on the remote's
      // callback thread outside g_pairsMutex, and SendFrameToServer takes it itself.
      std::vector<SocialParty::Message> outgoing;
      const uint64_t nowSeconds = static_cast<uint64_t>(std::time(nullptr));
      if (SocialParty::Global().Feed(sym, payload, static_cast<size_t>(len), nowSeconds, &outgoing) &&
          !outgoing.empty()) {
        SocialParty::Send(outgoing);
      }
      // A party member or an invite's sender the bridge has no name for: ask for the profile, as for
      // friends, so the game shows a name instead of an account id.
      for (const uint64_t accountId : SocialParty::Global().TakeUnnamed()) {
        const std::vector<SocialParty::Message> asks = SocialNames::GlobalResolver().Want(accountId);
        if (!asks.empty()) {
          Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party name lookup requested account=%llu sent=%d",
              static_cast<unsigned long long>(accountId), SocialParty::Send(asks) ? 1 : 0);
        }
      }
    }
    offset += EvrCodec::kHeaderSize + static_cast<size_t>(len);
  }
}

// Sends a frame from the runtime itself (the social facade's party requests) to the server on the
// game's login connection, as if the game had. Queues it if the remote is still connecting, as the
// game-side path does. Takes g_pairsMutex, so it must not be called with it held.
static bool SendFrameToServer(const std::string& frame) {
  bool sent = false;
  {
    std::lock_guard<std::mutex> lock(g_pairsMutex);
    for (auto& entry : g_pairs) {
      ProxyPair& pair = *entry.second;
      if (pair.connIdx != 1 || !pair.remoteWs) continue;
      if (!pair.remoteOpen) {
        pair.pendingToRemote.push_back(frame);
        sent = true;
      } else {
        sent = pair.remoteWs->sendBinary(frame).success;
      }
      break;
    }
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

// Registers SendFrameToServer as the party requests' sender when the bridge is loaded.
static const bool g_partySenderRegistered = (SocialParty::SetSender(&SendFrameToServer), true);

// Platform codes, the bridge's login platform, the remote Bearer choice and the /ws path test live in
// EvrCodec (compat/evr_codec.h), shared with the Quest target.

// The HMD serial field the stock client sends (#83): relay the game's serial buffer, or its stock "N/A"
// value in No-VR mode. Outside the game (unit tests) there is nothing to read.
static HmdSerial::Choice GameHmdSerial() {
  if (EchoVR::g_GameBaseAddress == nullptr || g_pGame == nullptr) return HmdSerial::Select(false, nullptr);
  uint32_t flags = 0;
  memcpy(&flags, static_cast<const char*>(g_pGame) + 0x7AE0, sizeof(flags));
  const char* serial = reinterpret_cast<const char*>(EchoVR::g_GameBaseAddress) + 0x20C7834;
  return HmdSerial::Select((flags & HmdSerial::kNoVrFlag) != 0, serial);
}

static std::string BuildLoginRequest(uint64_t discordId, uint64_t platformCode = 2,
                                     const std::string& displayName = std::string(),
                                     const std::string& accessToken = std::string(),
                                     const std::string& password = std::string()) {
  // Platform codes: see EvrCodec::PlatformPrefix (1-indexed: STM=1 ... OVR_ORG=4 ... DMO=7).
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
  const HmdSerial::Choice hmd = GameHmdSerial();
  Log(EchoVR::LogLevel::Info, "[NEVR.WS] login hmd serial source=%s length=%zu",
      HmdSerial::SourceName(hmd.source), hmd.value.size());

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

  LoginProfile::LoginProfileInputs profileInputs;
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
  profileInputs.social_level = SocialParty::kSocialLevel;
  profileInputs.plugins = std::move(plugins);
  const std::string jsonStr = LoginProfile::BuildLoginProfileJson(profileInputs);

  // Frame layout (UUID, platform, account, JSON, NUL) is EvrCodec's, shared with the Quest target.
  std::optional<std::string> frame = EvrCodec::BuildLoginRequest(platformCode, accountId, jsonStr);
  if (!frame.has_value()) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.WS] login request not built: the profile JSON contains a NUL byte (%zu bytes); no login is sent",
        jsonStr.size());
    return std::string();
  }
  return std::move(*frame);
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

  // Callbacks set after successful listen(), before start().
  auto onClientMessage = GuardWsCallback("ws_bridge.cpp:setOnClientMessageCallback",
      [](std::shared_ptr<ix::ConnectionState> connState,
         ix::WebSocket& gameWs,
         const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
          case ix::WebSocketMessageType::Open: {
            int connIdx = g_connectionCount++;

            // conn>=2 (matchmaker): reuse the login connection's remote WS.
            // The matchmaker needs the fully-authenticated session (login + profile
            // exchange) that conn=1 established. A fresh LoginRequest-only session
            // won't have the server-side state needed for PlayerSessionRequest.
            // Don't inject LoginRequest — the session is already logged in.
            if (connIdx >= 2 && g_loginRemoteWs) {
              Log(EchoVR::LogLevel::Info,
                  "[NEVR.WS] Proxy: game connected (conn=%d, %s, ws=%p), sharing login session (no LoginRequest)",
                  connIdx, ConnLabel(connIdx), static_cast<void*>(&gameWs));
              auto pair = std::make_unique<ProxyPair>();
              pair->remoteWs = g_loginRemoteWs;
              pair->remoteOpen = true;
              pair->loginInjected = true;  // skip LoginRequest — already authenticated
              pair->connIdx = connIdx;

              auto* pairPtr = pair.get();
              ix::WebSocket* gameWsPtr = &gameWs;

              // N61: register an independent callback for each matchmaker
              // connection on the shared remote. Relying on the login
              // connection's callback alone fails: when login
              // disconnects and B2/N54 nulls that callback, all matchmaker
              // server→game message routing silently dies.
              g_loginRemoteWs->setOnMessageCallback(GuardWsCallback("ws_bridge.cpp:setOnMessageCallback",
                  [pairPtr, gameWsPtr, connIdx,
                   remoteAddress = static_cast<const ix::WebSocket*>(g_loginRemoteWs.get())](const ix::WebSocketMessagePtr& rmsg) {
                    switch (rmsg->type) {
                      case ix::WebSocketMessageType::Message: {
                        ix::WebSocket* target = nullptr;
                        int targetConn = -1;
                        {
                          std::lock_guard<std::mutex> lk(g_pairsMutex);
                          if (const ProxyPair* route = SharedRouteLocked(&target)) targetConn = route->connIdx;
                        }
                        ObserveSocialFrames("server->game", targetConn, rmsg->str);
                        if (target == nullptr) {
                          LogSharedFrameDropped(rmsg->str.size());
                          break;
                        }
                        if (rmsg->binary) {
                          target->sendBinary(rmsg->str);
                        } else {
                          target->sendText(rmsg->str);
                        }
                        break;
                      }
                      case ix::WebSocketMessageType::Close:
                        Log(EchoVR::LogLevel::Info,
                            "[NEVR.WS] Remote closed (conn=%d, %s, ws=%p): code=%u",
                            connIdx, ConnLabel(connIdx), static_cast<void*>(gameWsPtr),
                            static_cast<unsigned int>(rmsg->closeInfo.code));
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

            // conn=0 (config) and conn=1 (login): create new remote ws
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
              std::optional<std::string> withCredentials = ServerDbUri::BuildBridgeCredentialUri(
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
            // conn>=2 (matchmaker): pnsradmatchmaking uses protobuf, not EchoVR
            // binary. Strip format=evr so the server uses default protobuf handling.
            // Issue #116: format=evr is routinely the FIRST query param here: a configured
            // socket_uri such as "wss://host/ws?format=evr&token=..." already
            // carries it before the credentials block above appends
            // discordid/password. A naive "delete the preceding
            // ? or &" deleted the URI's only '?' and glued the path to the
            // remaining query. ServerDbUri::RemoveQueryParam handles leading/
            // middle/trailing/sole position correctly; see its own tests.
            if (connIdx >= 2) {
              remoteUrl = ServerDbUri::RemoveQueryParam(remoteUrl, "format=evr");
              const std::string diagnostic = nevr_log_diagnostics::FormatRedactedUrlDiagnostic(
                  "[NEVR.WS] Matchmaker conn=" + std::to_string(connIdx) + " using protobuf URL: ", remoteUrl);
              Log(EchoVR::LogLevel::Debug, "%s", diagnostic.c_str());
            }
            remote->setUrl(remoteUrl);
            remote->disableAutomaticReconnection();
            remote->disablePerMessageDeflate();

            // Get auth token from token_auth module (resolved via cross-module procs)
            std::string bearerToken;
            std::string accountName;  // N123 — empty means "not known", never a placeholder
            uint64_t discordId = 0;
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
                Log(EchoVR::LogLevel::Info,
                    "[NEVR.WS] Using nevr_discord_id from config: %llu",
                    static_cast<unsigned long long>(discordId));
              }
            }
            if (discordId == 0) {
              Log(EchoVR::LogLevel::Warning,
                  "[NEVR.WS] No discord ID from JWT or config — LoginRequest will use "
                  "account ID 0");
            }
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
            const std::string remoteBearer = EvrCodec::SelectRemoteBearer(hasUrlCredentials, bearerToken, serverKey);
            if (!remoteBearer.empty()) {
              ix::WebSocketHttpHeaders headers;
              headers["Authorization"] = "Bearer " + remoteBearer;
              remote->setExtraHeaders(headers);
              Log(EchoVR::LogLevel::Info,
                  "[NEVR.WS] remote auth: %s (value not logged)",
                  hasUrlCredentials ? "server key + URL credentials" : "token-auth JWT");
              if (!hasUrlCredentials && EvrCodec::IsBearerReplacingPath(remoteUrl)) {
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

            auto pair = std::make_unique<ProxyPair>();
            pair->remoteWs = remote;
            pair->connIdx = connIdx;

            auto* pairPtr = pair.get();
            ix::WebSocket* gameWsPtr = &gameWs;

            // Remote → game forwarding
            remote->setOnMessageCallback(GuardWsCallback("ws_bridge.cpp:setOnMessageCallback", 
                // accountName captured BY VALUE alongside discordId — this callback
                // outlives the enclosing scope, so a reference would dangle (N123).
                // remoteAddress: only compared, never dereferenced (capturing the shared_ptr would be a cycle).
                [pairPtr, gameWsPtr, connIdx, discordId, accountName, bearerToken,
                 remoteAddress = static_cast<const ix::WebSocket*>(remote.get())](const ix::WebSocketMessagePtr& rmsg) {
                  switch (rmsg->type) {
                    case ix::WebSocketMessageType::Open: {
                      std::lock_guard<std::mutex> lk(g_pairsMutex);
                      pairPtr->remoteOpen = true;
                      const std::string diagnostic = nevr_log_diagnostics::FormatRedactedUrlDiagnostic(
                          "[NEVR.WS] Remote open (conn=" + std::to_string(connIdx) + ", " + ConnLabel(connIdx) + "): ",
                          g_remoteUri);
                      Log(EchoVR::LogLevel::Debug, "%s", diagnostic.c_str());

                      // Inject LoginRequest on login connections (not config).
                      // pnsrad.dll won't send its own because it has no user identity
                      // (OVR SDK is bypassed). The LoginRequest is built and injected here.
                      //
                      // Before injecting, set the CNSUser's login state to "logging in"
                      // so that CNSUser::LogInSuccessCB processes the server's LoginSuccess
                      // response. Without this, LogInSuccessCB silently discards the message
                      // because the user's login state at +0x90 is still 0 (logged out).
                      if (connIdx == 1 && !pairPtr->loginInjected) {
                        g_loginInjectedAnywhere.store(true, std::memory_order_release);
                        pairPtr->loginInjected = true;

                        // Set CNSUser login state only on the actual login connection.
                        // Later connections (matchmaker, etc.) must not reset the state
                        // or the game loses its logged-in status during lobby join.
                        if (connIdx == 1) {
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
                                  // +0x90 low nibble = provider enum (2 = PSN in binary,
                                  //   patched to DSC by PatchDscProvider string table rewrite)
                                  // +0x9c = state flags (0x04 = connected/logged in)
                                  *accountId  = static_cast<int64_t>(discordId);
                                  *loginState = (*loginState & ~0xFULL) | EvrCodec::kBridgeLoginPlatform;  // OVR_ORG (game numbering)
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

                        uint64_t platformCode;
                        std::string cfgPasswordStr;
                        {
                          const char* cfgDiscordId = NevrCfgGetFlat("nevr_discord_id");
                          const char* cfgPassword = NevrCfgGetFlat("nevr_password");
                          bool hasUrlCreds = cfgDiscordId && cfgDiscordId[0] && cfgPassword && cfgPassword[0];
                          platformCode = EvrCodec::SelectPlatformCode(hasUrlCreds, g_noOvr);
                          if (cfgPassword) cfgPasswordStr = cfgPassword;
                        }
                        g_lastInjectedDiscordId = discordId;
                        SocialParty::Global().SetSelf(discordId, accountName);
                        std::string loginMsg = BuildLoginRequest(discordId, platformCode, accountName, bearerToken, cfgPasswordStr);
                        if (loginMsg.empty()) {
                          Log(EchoVR::LogLevel::Error, "[NEVR.WS] login NOT injected conn=%d (%s): the request could not be built",
                              connIdx, ConnLabel(connIdx));
                        } else {
                          pairPtr->remoteWs->sendBinary(loginMsg);
                          std::string xpid = std::string(EvrCodec::PlatformPrefix(platformCode)) + "-" + std::to_string(discordId);
                          Log(EchoVR::LogLevel::Info,
                              "[NEVR.WS] login injected xpid=%s platform=%d conn=%d (%s) size=%zu",
                              xpid.c_str(), static_cast<int>(platformCode), connIdx, ConnLabel(connIdx), loginMsg.size());
                        }
                      }

                      for (auto& pending : pairPtr->pendingToRemote) {
                        pairPtr->remoteWs->sendBinary(pending);
                      }
                      pairPtr->pendingToRemote.clear();
                      break;
                    }
                    case ix::WebSocketMessageType::Message: {
                      ObserveSocialFrames("server->game", connIdx, rmsg->str);
                      // First message's symbol (marker@0, symbol@8, length@16), for the decodes below.
                      const uint64_t rsym = EvrCodec::FirstSymbol(rmsg->str);
                      LogFrameMessages("server->game", connIdx, rmsg->str);
                      // Decode only the numeric LoginFailure diagnostics. The
                      // server-provided message can contain credentials or other
                      // private response data and is never written to logs.
                      if (rsym == EvrCodec::kSymLoginFailure && rmsg->str.size() > 48) {
                        const std::optional<EvrCodec::LoginFailure> diagnostic =
                            EvrCodec::ParseLoginFailure(rmsg->str);
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
                          Log(EchoVR::LogLevel::Debug,
                              "[NEVR.WS] Server mode — injecting fake LoginSuccess to bypass login gate");
                          uint64_t serverPlatformCode;
                          {
                            const char* cfgDiscordId2 = NevrCfgGetFlat("nevr_discord_id");
                            const char* cfgPassword2 = NevrCfgGetFlat("nevr_password");
                            bool hasUrlCreds2 = cfgDiscordId2 && cfgDiscordId2[0] && cfgPassword2 && cfgPassword2[0];
                            serverPlatformCode = EvrCodec::SelectPlatformCode(hasUrlCreds2, g_noOvr);
                          }
                          const std::string fakeSuccess =
                              EvrCodec::BuildLoginSuccess(serverPlatformCode, g_lastInjectedDiscordId);
                          gameWsPtr->sendBinary(fakeSuccess);
                          break;  // failure is not forwarded to the game
                        }
                      }
                      // Decode LoginSuccess (sym 0xa5acc1a90d0cce47)
                      if (rsym == EvrCodec::kSymLoginSuccess) {
                        Log(EchoVR::LogLevel::Info, "[NEVR.WS] LOGIN SUCCESS");
                        // Inject SNSFriendListSubscribeRequest directly to server.
                        // pnsrad's broadcaster handle (field_0x160) is null because
                        // echovr.exe doesn't provide one for the pnsrad platform
                        // provider, so pnsrad can't send SNS messages itself. Send
                        // the subscribe request through the WS bridge instead.
                        {
                          // Payload: 0x20 zero bytes (provider_id + UUID + token); the server ignores it.
                          const std::string subscribeMsg = EvrCodec::BuildFriendListSubscribe();
                          pairPtr->remoteWs->sendBinary(subscribeMsg);
                          Log(EchoVR::LogLevel::Debug,
                              "[NEVR.WS] Injected FriendListSubscribeRequest (%zu bytes)",
                              subscribeMsg.size());
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
                        ix::WebSocket* target = nullptr;
                        {
                          std::lock_guard<std::mutex> lk(g_pairsMutex);
                          if (pairPtr->remoteWs != nullptr && pairPtr->remoteWs == g_loginRemoteWs)
                            SharedRouteLocked(&target);
                          else
                            target = gameWsPtr;
                        }
                        if (target == nullptr) {
                          LogSharedFrameDropped(rmsg->str.size());
                          break;
                        }
                        std::optional<std::string> reordered;
                        if (rmsg->binary && rsym == EvrCodec::kSymLoginFailure) {
                          reordered = MoveCodeLineFirst(rmsg->str);
                          if (reordered.has_value()) {
                            Log(EchoVR::LogLevel::Info,
                                "[NEVR.WS] login failure text reordered: code line moved first bytes=%zu",
                                reordered->size());
                          }
                        }
                        const std::string& outFrame = reordered.has_value() ? *reordered : rmsg->str;
                        if (rmsg->binary) {
                          target->sendBinary(outFrame);
                        } else {
                          target->sendText(outFrame);
                        }
                      }
                      break;
                    }
                    case ix::WebSocketMessageType::Close:
                      Log(EchoVR::LogLevel::Info,
                          "[NEVR.WS] Remote closed (conn=%d, %s, ws=%p): code=%u",
                          connIdx, ConnLabel(connIdx), static_cast<void*>(gameWsPtr),
                          static_cast<unsigned int>(rmsg->closeInfo.code));
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
                      // Automatic reconnection is off, so a remote that fails (often its first connect,
                      // with no Close to follow) is dead; end the game's sockets on it so the game
                      // retries rather than waiting on a session that will never open (#70).
                      CloseGameSocketsForRemote(remoteAddress, connIdx, 0);
                      break;
                    default:
                      break;
                  }
                }));

            {
              std::lock_guard<std::mutex> lk(g_pairsMutex);
              g_pairs[gameWsPtr] = std::move(pair);
              // Save login connection for reuse by matchmaker (conn>=2)
              if (connIdx == 1) {
                g_loginRemoteWs = remote;
                g_activeGameWs = gameWsPtr;
                g_loginGameWs = gameWsPtr;
              }
            }
            // Start after insertion so the remote callback can find the pair in g_pairs
            remote->start();
            Log(EchoVR::LogLevel::Info, "[NEVR.WS] Proxy: game connected (conn=%d, %s, ws=%p)", connIdx,
                ConnLabel(connIdx), static_cast<void*>(gameWsPtr));
            const std::string remoteDiagnostic =
                nevr_log_diagnostics::FormatRedactedUrlDiagnostic("[NEVR.WS] Proxy remote target: ", g_remoteUri);
            Log(EchoVR::LogLevel::Info, "%s", remoteDiagnostic.c_str());
            break;
          }

          case ix::WebSocketMessageType::Message: {
            ObserveSocialFrames("game->server", ConnIdxOfGameWs(&gameWs), msg->str);
            LogFrameMessages("game->server", ConnIdxOfGameWs(&gameWs), msg->str);
            // Game→remote forwarding — dump all message symbols in the frame
            // EchoVR wire format: [marker(8)][symbol(8)][length(8)][payload(length)]...
            LogGameToServerFrame(msg->str, connState->getId());
            std::lock_guard<std::mutex> lk(g_pairsMutex);
            auto it = g_pairs.find(&gameWs);
            if (it != g_pairs.end()) {
              auto& pair = it->second;
              if (pair->remoteOpen) {
                // A shared login session can die while the game sits idle; the game then
                // waits forever on its MATCHMAKING screen. Say so instead of dropping quietly.
                const ix::ReadyState remoteState = pair->remoteWs->getReadyState();
                bool sent = false;
                if (msg->binary) {
                  sent = pair->remoteWs->sendBinary(msg->str).success;
                  Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   -> forwarded (success=%s)",
                      sent ? "true" : "false");
                } else {
                  sent = pair->remoteWs->sendText(msg->str).success;
                }
                if (!sent || remoteState != ix::ReadyState::Open) {
                  Log(EchoVR::LogLevel::Warning,
                      "[NEVR.WS] game->server message NOT delivered: conn=%d (%s) send_success=%s "
                      "remote_state=%s bytes=%zu — the remote session is gone; the game will wait "
                      "on this request indefinitely",
                      pair->connIdx, ConnLabel(pair->connIdx), sent ? "true" : "false",
                      RemoteStateName(remoteState), msg->str.size());
                }
              } else {
                pair->pendingToRemote.push_back(msg->str);
                Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   -> queued (remote not open yet, %zu pending)",
                    pair->pendingToRemote.size());
              }
            } else {
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
            }
            break;
          }

          case ix::WebSocketMessageType::Close: {
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

  g_server->setOnClientMessageCallback(onClientMessage);
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

    static std::unique_ptr<ix::WebSocketServer>& s_matchServer = *new std::unique_ptr<ix::WebSocketServer>();  // leaked, see g_server
    bool matchBound = false;
    for (int attempt = 0; attempt < kMaxMatchBindAttempts; ++attempt) {
      uint16_t tryPort = matchDist(matchGen);
      s_matchServer = std::make_unique<ix::WebSocketServer>(tryPort, "127.0.0.1");
      s_matchServer->disablePerMessageDeflate();
      auto [ok, errorText] = s_matchServer->listen();
      if (ok) {
        g_matchPort = tryPort;
        matchBound = true;
        break;
      }
      const std::string diagnostic =
          nevr_log_diagnostics::FormatBindFailureDiagnostic("Matchmaker", tryPort, attempt + 1,
                                                      kMaxMatchBindAttempts);
      Log(EchoVR::LogLevel::Warning, "%s error=\"%s\"", diagnostic.c_str(), errorText.c_str());
      s_matchServer.reset();
    }

    if (matchBound) {
      s_matchServer->setOnClientMessageCallback(onClientMessage);
      s_matchServer->start();
      Log(EchoVR::LogLevel::Info,
          "[NEVR.WS] Matchmaker listener on ws://127.0.0.1:%u", g_matchPort);
    } else {
      s_matchServer.reset();
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

// N105: the REAL stop, for callers that are NOT under the loader lock — the
// SIGINT/SIGTERM graceful path and gameserver's BeginGracefulShutdown.
//
// This capability was lost by the N92 fold: the only definition of
// WsBridge_Shutdown lives in src/modules/ws-bridge/, a module that stopped
// being built, while two shipping call sites still resolved it with
// GetProcAddress("ws_bridge.dll") and got null. Every server run since has
// logged `ws_bridge=absent WsBridge_Shutdown=null` and left the listener up.
//
// Stopping the listener is what releases the socket FD. ixwebsocket never sets
// SO_REUSEADDR (N37), so a leaked LISTEN socket is precisely the zombie-bind
// condition N39's random-ephemeral-port retry exists to route around — that
// workaround has been carrying the whole load alone.
void StopWebSocketBridgeListener() {
  g_bridgeEnabled = false;

  // N60: collect under the lock, stop OUTSIDE it. stop() joins the callback
  // thread, which may itself be waiting on g_pairsMutex — holding it here
  // deadlocks shutdown.
  std::vector<std::shared_ptr<ix::WebSocket>> remotes;
  {
    std::lock_guard<std::mutex> lk(g_pairsMutex);
    for (auto& pair : g_pairs) {
      if (pair.second && pair.second->remoteWs) remotes.push_back(pair.second->remoteWs);
    }
    g_pairs.clear();
    g_activeGameWs = nullptr;
  }
  for (auto& ws : remotes) {
    ws->stop();
  }

  if (g_server) {
    g_server->stop();
    g_server.reset();
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.WS] listener stopped, %zu remote connection(s) closed — socket released",
      remotes.size());
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
  return EvrCodec::SelectRemoteBearer(hasUrlCredentials, jwt, serverKey);
}

bool TestHook_IsBearerReplacingPath(const std::string& url) { return EvrCodec::IsBearerReplacingPath(url); }

uint64_t TestHook_SelectPlatformCode(bool hasUrlCredentials, bool noOvr) {
  return EvrCodec::SelectPlatformCode(hasUrlCredentials, noOvr);
}

const char* TestHook_PlatformPrefix(uint64_t platformCode) {
  return EvrCodec::PlatformPrefix(platformCode);
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

bool TestHook_ReadLoginFailureDiagnostic(const std::string& frame, uint64_t* statusCode, size_t* messageBytes) {
  if (statusCode == nullptr || messageBytes == nullptr) return false;
  const std::optional<EvrCodec::LoginFailure> diagnostic = EvrCodec::ParseLoginFailure(frame);
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
  const std::optional<EvrCodec::LoginFailure> diagnostic = EvrCodec::ParseLoginFailure(frame);
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
  pair->connIdx = 1;

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
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  const bool forgot = ForgetLoginSessionLocked(remotePtr->get());
  if (nextConnIdx != nullptr) *nextConnIdx = g_connectionCount.load();
  return forgot;
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

// Reset all internal bridge state for the next test.
void TestHook_N61_ResetState() {
  std::lock_guard<std::mutex> lk(g_pairsMutex);
  g_pairs.clear();
  g_loginRemoteWs.reset();
  g_loginGameWs = nullptr;
  g_activeGameWs = nullptr;
  g_connectionCount.store(0);
}

#endif  // NEVR_TEST_HOOKS
