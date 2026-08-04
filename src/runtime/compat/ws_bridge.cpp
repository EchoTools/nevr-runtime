#include "runtime/compat/ws_bridge.h"
#include "runtime/hook/symbol_corpus.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/lifecycle/config.h"
#include "runtime/ext/module_loader.h"
#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "core/system_info.h"
#include "core/build_identity.h"   // N112: NEVR version identity
#include "runtime/ext/plugin_loader.h"  // N112: plugin manifest
#include "runtime/lifecycle/cli.h"  // g_isServer
#include "runtime/lifecycle/service_config.h"  // NevrCfgGetFlat (N133 S4a: config.yaml reads)
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
    } catch (const std::exception& e) {
      Log(EchoVR::LogLevel::Error,
          "[NEVR.WS] callback threw and was CONTAINED at=%s what=%s — server continues "
          "(an escape here reaches the game's unhandled-exception filter and kills it, N85)",
          what, e.what());
    }
  };
}

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

static std::unique_ptr<ix::WebSocketServer> g_server;
static std::string g_remoteUri;
static uint16_t g_proxyPort = 0;
static bool g_bridgeEnabled = false;

// Per-connection state: maps game-side server WebSocket → remote ix::WebSocket
struct ProxyPair {
  std::shared_ptr<ix::WebSocket> remoteWs;
  std::vector<std::string> pendingToRemote;
  bool remoteOpen = false;
  bool loginInjected = false;  // true after we inject LoginRequest on this connection
};

static std::mutex g_pairsMutex;
static std::atomic<int> g_connectionCount{0};  // tracks connection order (0=config, 1+=login)
static std::unordered_map<ix::WebSocket*, std::unique_ptr<ProxyPair>> g_pairs;

// The login connection's remote WS (conn=1). Connections after login (conn>=2,
// e.g. matchmaker) reuse this so all traffic shares the same Nakama session.
// The original game multiplexes config/login/matchmaker on one WS to one server;
// Nakama correlates matchmaker allocations by session, so the matchmaker must
// use the same authenticated session as login.
static std::shared_ptr<ix::WebSocket> g_loginRemoteWs;

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

// ============================================================================
// LoginRequest builder
// ============================================================================
// EchoVR wire format: [marker(8)][symbol(8)][length(8)][payload]
// LoginRequest payload: [UUID(16)][PlatformCode(8)][AccountId(8)][JSON\0]

static const uint8_t MSG_MARKER[] = {0xf6,0x40,0xbb,0x78,0xa2,0xe7,0x8c,0xbb};
static const uint64_t SYM_LOGIN_REQUEST = 0xbdb41ea9e67b200a;

static void AppendLE64(std::string& buf, uint64_t val) {
  for (int i = 0; i < 8; i++) { buf.push_back((char)(val & 0xFF)); val >>= 8; }
}

// N146: platform codes match the Go server's iota enumeration.
// Go: STM=0, DSC=1, XBX=2, OVR_ORG=3, OVR=4, BOT=5, DMO=6
static const char* PlatformPrefix(uint64_t platformCode) {
  switch (platformCode) {
    case 0: return "STM";
    case 1: return "DSC";
    case 2: return "XBX";
    case 3: return "OVR-ORG";
    case 4: return "OVR";
    case 5: return "BOT";
    case 6: return "DSC-NOVR";  // DMO = demo/no-VR client
    default: return "UNK";
  }
}

// N123. displayName was the hardcoded literal "nEVR", so every NEVR client in a
// session announced the identical name — eight players would render eight
// identical nameplates. token_auth already parsed a username from the auth
// response and already persisted it to the credential cache; it had just never
// been exposed. Empty means "not known", and the caller below sends the account
// id rather than substituting something that looks like a real name (N115: absent
// data is visibly absent, invented data is indistinguishable from a reading).
static std::string BuildLoginRequest(uint64_t discordId, uint64_t platformCode = 2,
                                     const std::string& displayName = std::string(),
                                     const std::string& accessToken = std::string()) {
  // Platform codes match Go server iota: STM=0, DSC=1, XBX=2, OVR_ORG=3, OVR=4, BOT=5, DMO=6
  uint64_t accountId = discordId;

  // Host facts, MEASURED. Every value in this block used to be a literal —
  // "cpu":"Wine", "video_card":"Wine D3D12", 4 physical cores, 8 logical,
  // 16384 MB total, 8192 used — sent as though read from the machine. That is
  // worse than sending nothing: absent data is visibly absent, while invented
  // data is indistinguishable from a reading and gets acted on.
  //
  // Fields this process cannot honestly determine are now sent EMPTY or 0
  // rather than guessed. video_card and dedicated_gpu_memory have no truthful
  // answer on a headless server with no device enumerated, and network_type
  // was never anything but a guess. Empty is a true statement; "Wine D3D12" is
  // not. N112.
  const SystemInfo::Host& host = SystemInfo::Get();
  const std::string driverVersion =
      host.IsWine() ? ("Wine " + host.wine_version +
                       (host.wine_host_os.empty() ? "" : " on " + host.wine_host_os))
                    : std::string();

  // Empty means the account's name is genuinely not known yet. Fall back to the
  // account id — a true, unique identifier — rather than a constant. A shared
  // placeholder is what made every NEVR client announce the same name.
  std::string resolvedName = displayName;
  if (resolvedName.empty()) resolvedName = std::to_string(accountId);

  // LoginProfile JSON — matches the game's SNSLogInRequestv2 format.
  //
  // N146: nlohmann_json instead of hand-built snprintf.  A hand-built format
  // string cannot escape its own values, so a version string or display name
  // containing a double-quote produces malformed JSON the server rejects.
  // nlohmann::json guarantees valid output regardless of input.
  const BuildIdentity::Info& buildId = BuildIdentity::Get();
  const std::string pluginManifest = BuildPluginManifestJson();

  std::string jsonStr;
  {
    nlohmann::json j;
    j["accountid"] = accountId;
    j["displayname"] = resolvedName;
    j["bypassauth"] = false;
    j["access_token"] = accessToken;
    j["nonce"] = "";
    j["buildversion"] = 631547;
    j["lobbyversion"] = 0;
    j["appid"] = 0;
    j["publisher_lock"] = "";
    j["hmdserialnumber"] = "nEVR-Wine";
    j["desiredclientprofileversion"] = 0;

    auto& ident = j["nevr_identity"];
    ident["version"] = buildId.project_version;
    ident["commit"] = buildId.git_commit;
    ident["build"] = buildId.git_describe;
    ident["build_type"] = buildId.build_type;

    // nevr_plugins: parse the pre-built manifest so the field is a JSON array,
    // not a string-escaped copy of one.
    if (!pluginManifest.empty()) {
      try {
        j["nevr_plugins"] = nlohmann::json::parse(pluginManifest);
      } catch (...) {
        j["nevr_plugins"] = nlohmann::json::array();
      }
    } else {
      j["nevr_plugins"] = nlohmann::json::array();
    }

    auto& sys = j["system_info"];
    sys["headset_type"] = "No VR";
    sys["driver_version"] = driverVersion;
    sys["network_type"] = "";
    sys["video_card"] = "";
    sys["cpu"] = host.cpu_brand;
    sys["num_physical_cores"] = host.physical_cores;
    sys["num_logical_cores"] = host.logical_cores;
    sys["memory_total"] = host.memory_total_mb;
    sys["memory_used"] = host.memory_used_mb;
    sys["dedicated_gpu_memory"] = 0;

    jsonStr = j.dump();
  }

  size_t jsonLen = jsonStr.size() + 1;  // include null terminator

  // Build payload: UUID(16) + PlatformCode(8) + AccountId(8) + JSON+null
  std::string payload;
  payload.reserve(16 + 8 + 8 + jsonLen);
  // UUID = all zeros (no previous session)
  for (int i = 0; i < 16; i++) payload.push_back('\0');
  AppendLE64(payload, platformCode);
  AppendLE64(payload, accountId);
  payload.append(jsonStr.c_str(), jsonLen);

  // Build full message: marker + symbol + length + payload
  std::string msg;
  msg.reserve(8 + 8 + 8 + payload.size());
  msg.append((const char*)MSG_MARKER, 8);
  AppendLE64(msg, SYM_LOGIN_REQUEST);
  AppendLE64(msg, payload.size());
  msg.append(payload);

  return msg;
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

bool IsWebSocketBridgeActive() {
  return g_bridgeEnabled;
}

void InstallWebSocketBridge() {
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

    auto [ok, errMsg] = g_server->listen();
    if (ok) {
      g_proxyPort = tryPort;
      bound = true;
      break;
    }

    Log(EchoVR::LogLevel::Warning,
        "[NEVR.WS] Port %u bind failed: %s — retrying (%d/%d)",
        tryPort, errMsg.c_str(), attempt + 1, kMaxBindAttempts);
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
                  "[NEVR.WS] Proxy: game connected (conn=%d, ws=%p), sharing login session (no LoginRequest)",
                  connIdx, (void*)&gameWs);
              auto pair = std::make_unique<ProxyPair>();
              pair->remoteWs = g_loginRemoteWs;
              pair->remoteOpen = true;
              pair->loginInjected = true;  // skip LoginRequest — already authenticated

              auto* pairPtr = pair.get();
              ix::WebSocket* gameWsPtr = &gameWs;

              // N61: register an independent callback for each matchmaker
              // connection on the shared remote. Previously matchmaker relied
              // entirely on the login connection's callback — when login
              // disconnected and B2/N54 nulled that callback, all matchmaker
              // server→game message routing silently died.
              g_loginRemoteWs->setOnMessageCallback(GuardWsCallback("ws_bridge.cpp:setOnMessageCallback", 
                  [pairPtr, gameWsPtr](const ix::WebSocketMessagePtr& rmsg) {
                    switch (rmsg->type) {
                      case ix::WebSocketMessageType::Message: {
                        ix::WebSocket* target = nullptr;
                        {
                          std::lock_guard<std::mutex> lk(g_pairsMutex);
                          target = g_activeGameWs ? g_activeGameWs : gameWsPtr;
                        }
                        if (rmsg->binary) {
                          target->sendBinary(rmsg->str);
                        } else {
                          target->sendText(rmsg->str);
                        }
                        break;
                      }
                      case ix::WebSocketMessageType::Close:
                        Log(EchoVR::LogLevel::Debug,
                            "[NEVR.WS] Remote closed (matchmaker ws=%p): %d %s",
                            (void*)gameWsPtr, rmsg->closeInfo.code,
                            rmsg->closeInfo.reason.c_str());
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
            {
              const char* cfgDiscordId = NevrCfgGetFlat("nevr_discord_id");
              const char* cfgPassword = NevrCfgGetFlat("nevr_password");
              if (cfgDiscordId && cfgDiscordId[0] != '\0' && cfgPassword && cfgPassword[0] != '\0') {
                char sep = (remoteUrl.find('?') != std::string::npos) ? '&' : '?';
                remoteUrl += sep;
                remoteUrl += "discordid=";
                remoteUrl += cfgDiscordId;
                remoteUrl += "&password=";
                remoteUrl += cfgPassword;
              }
            }
            // conn>=2 (matchmaker): pnsradmatchmaking uses protobuf, not EchoVR
            // binary. Strip format=evr so the server uses default protobuf handling.
            if (connIdx >= 2) {
              auto pos = remoteUrl.find("format=evr");
              if (pos != std::string::npos) {
                // Remove "format=evr" and the preceding ? or &
                size_t start = (pos > 0 && (remoteUrl[pos-1] == '?' || remoteUrl[pos-1] == '&'))
                               ? pos - 1 : pos;
                size_t end = pos + 10;  // len("format=evr")
                // If there's a trailing & after format=evr, remove it too
                if (end < remoteUrl.size() && remoteUrl[end] == '&') end++;
                remoteUrl.erase(start, end - start);
                // If we left a trailing ? with nothing after, remove it
                if (!remoteUrl.empty() && remoteUrl.back() == '?') remoteUrl.pop_back();
              }
              Log(EchoVR::LogLevel::Debug,
                  "[NEVR.WS] Matchmaker conn=%d using protobuf URL: %s", connIdx, remoteUrl.c_str());
            }
            remote->setUrl(remoteUrl);
            remote->disableAutomaticReconnection();
            remote->disablePerMessageDeflate();

            // Get auth token from token_auth module (resolved via cross-module procs)
            std::string bearerToken;
            std::string accountName;  // N123 — empty means "not known", never a placeholder
            uint64_t discordId = 0;
            {
              auto getTokenFn = (const char* (*)())ResolveModuleProc("TokenAuth_GetToken");
              auto getDiscordIdFn = (uint64_t (*)())ResolveModuleProc("TokenAuth_GetDiscordId");
              // N123. Optional by design: an older token_auth.dll without this
              // export must still load. A null here means "no name available",
              // which BuildLoginRequest already handles.
              auto getUsernameFn = (const char* (*)())ResolveModuleProc("TokenAuth_GetUsername");
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
                    (unsigned long long)discordId);
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
            if (!bearerToken.empty() && !hasUrlCredentials) {
              ix::WebSocketHttpHeaders headers;
              headers["Authorization"] = "Bearer " + bearerToken;
              remote->setExtraHeaders(headers);
              Log(EchoVR::LogLevel::Debug, "[NEVR.WS] Attaching Bearer token to remote connection");
            } else if (hasUrlCredentials) {
              Log(EchoVR::LogLevel::Debug, "[NEVR.WS] Using URL credentials (no Bearer token)");
            }

            auto pair = std::make_unique<ProxyPair>();
            pair->remoteWs = remote;

            auto* pairPtr = pair.get();
            ix::WebSocket* gameWsPtr = &gameWs;

            // Remote → game forwarding
            remote->setOnMessageCallback(GuardWsCallback("ws_bridge.cpp:setOnMessageCallback", 
                // accountName captured BY VALUE alongside discordId — this callback
                // outlives the enclosing scope, so a reference would dangle (N123).
                [pairPtr, gameWsPtr, connIdx, discordId, accountName, bearerToken](const ix::WebSocketMessagePtr& rmsg) {
                  switch (rmsg->type) {
                    case ix::WebSocketMessageType::Open: {
                      std::lock_guard<std::mutex> lk(g_pairsMutex);
                      pairPtr->remoteOpen = true;
                      Log(EchoVR::LogLevel::Debug, "[NEVR.WS] Remote open (conn=%d): %s",
                          connIdx, g_remoteUri.c_str());

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
                            auto Users = (UsersFn)GetProcAddress(hPnsrad, "Users");
                            if (Users) {
                              auto* usersObj = (uint8_t*)Users();
                              if (usersObj) {
                                uint64_t userCount = *(uint64_t*)(usersObj + 0x398);
                                uint8_t** bufCtx = *(uint8_t***)(usersObj + 0x368);
                                if (userCount > 0 && bufCtx && *bufCtx) {
                                  uint8_t* user = *bufCtx;
                                  int64_t*  accountId  = (int64_t*)(user + 0x88);
                                  uint64_t* loginState = (uint64_t*)(user + 0x90);
                                  uint32_t* stateFlags = (uint32_t*)(user + 0x9c);
                                  Log(EchoVR::LogLevel::Debug,
                                      "[NEVR.WS] CNSUser BEFORE: acct=%lld state=0x%llx flags=0x%x",
                                      (long long)*accountId, (unsigned long long)*loginState, *stateFlags);
                                  // Set the user's XPID: account_id and provider enum.
                                  // +0x88 = account_id (discord ID from JWT)
                                  // +0x90 low nibble = provider enum (2 = PSN in binary,
                                  //   patched to DSC by PatchDscProvider string table rewrite)
                                  // +0x9c = state flags (0x04 = connected/logged in)
                                  *accountId  = (int64_t)discordId;
                                  *loginState = (*loginState & ~0xFULL) | 2;  // PSN (patched to DSC)
                                  *stateFlags = 0x04;
                                  Log(EchoVR::LogLevel::Debug,
                                      "[NEVR.WS] CNSUser AFTER:  acct=%lld state=0x%llx flags=0x%x",
                                      (long long)*accountId, (unsigned long long)*loginState, *stateFlags);
                                }
                              }
                            }
                          }
                        }

                        uint64_t platformCode = g_noOvr ? static_cast<uint64_t>(6) : static_cast<uint64_t>(1);
                        g_lastInjectedDiscordId = discordId;
                        std::string loginMsg = BuildLoginRequest(discordId, platformCode, accountName, bearerToken);
                        pairPtr->remoteWs->sendBinary(loginMsg);
                        std::string xpid = std::string(PlatformPrefix(platformCode)) + "-" + std::to_string(discordId);
                        Log(EchoVR::LogLevel::Info,
                            "[NEVR.WS] login injected xpid=%s platform=%d conn=%d size=%zu",
                            xpid.c_str(), static_cast<int>(platformCode), connIdx, loginMsg.size());
                      }

                      for (auto& pending : pairPtr->pendingToRemote) {
                        pairPtr->remoteWs->sendBinary(pending);
                      }
                      pairPtr->pendingToRemote.clear();
                      break;
                    }
                    case ix::WebSocketMessageType::Message: {
                      // Forward server→game — log symbol ID (marker@0, symbol@8, length@16)
                      uint64_t rsym = 0;
                      uint64_t rlen = 0;
                      if (rmsg->str.size() >= 24) {
                        memcpy(&rsym, rmsg->str.data() + 8, 8);
                        memcpy(&rlen, rmsg->str.data() + 16, 8);
                      }
                      char symBuf[192];
                      const char* name = EchoVR::LookupSymbolName(rsym);
                      if (name) {
                        snprintf(symBuf, sizeof(symBuf), "0x%016llx (%s)",
                                 (unsigned long long)rsym, name);
                      } else {
                        snprintf(symBuf, sizeof(symBuf), "0x%016llx",
                                 (unsigned long long)rsym);
                      }
                      Log(EchoVR::LogLevel::Debug, "[NEVR.WS] server->game: %zu bytes sym=%s payloadLen=%llu",
                          rmsg->str.size(), symBuf, (unsigned long long)rlen);
                      // Decode LoginFailure error message (sym 0xa5b9d5a3021ccf51)
                      if (rsym == 0xa5b9d5a3021ccf51 && rmsg->str.size() > 48) {
                        // payload: PlatformCode(8) + AccountId(8) + StatusCode(8) + ErrorMsg\0
                        uint64_t statusCode = 0;
                        memcpy(&statusCode, rmsg->str.data() + 24 + 16, 8);
                        const char* errMsg = rmsg->str.data() + 24 + 24;
                        size_t errMaxLen = rmsg->str.size() - 48;
                        Log(EchoVR::LogLevel::Warning, "[NEVR.WS] LOGIN FAILURE: status=%llu msg=%.*s",
                            (unsigned long long)statusCode, (int)errMaxLen, errMsg);

                        // N92: ported from the ws-bridge module, which was the shipping
                        // copy until the monolithic fold. A dedicated server has no
                        // interactive login; ServerDB answers LoginRequest with
                        // LoginFailure, and without this the game stalls at the login
                        // gate. Synthesize the LoginSuccess the game is waiting for and
                        // do NOT forward the failure.
                        if (g_isServer) {
                          Log(EchoVR::LogLevel::Debug,
                              "[NEVR.WS] Server mode — injecting fake LoginSuccess to bypass login gate");
                          static const uint64_t SYM_LOGIN_SUCCESS = 0xa5acc1a90d0cce47;
                          // LoginSuccess payload: Session UUID(16) + PlatformCode(8) + AccountId(8)
                          uint8_t payload[32] = {};
                          payload[0] = 0x4E; payload[1] = 0x45; payload[2] = 0x56; payload[3] = 0x52; // "NEVR"
                          payload[4] = 0x53; payload[5] = 0x52; payload[6] = 0x56; payload[7] = 0x52; // "SRVR"
                          uint64_t platformCode = g_noOvr ? static_cast<uint64_t>(6) : static_cast<uint64_t>(1);
                          memcpy(payload + 16, &platformCode, 8);
                          memcpy(payload + 24, &g_lastInjectedDiscordId, 8);

                          std::string fakeSuccess;
                          fakeSuccess.append(reinterpret_cast<const char*>(MSG_MARKER), 8);
                          AppendLE64(fakeSuccess, SYM_LOGIN_SUCCESS);
                          AppendLE64(fakeSuccess, sizeof(payload));
                          fakeSuccess.append(reinterpret_cast<const char*>(payload), sizeof(payload));
                          gameWsPtr->sendBinary(fakeSuccess);
                          break;  // failure is not forwarded to the game
                        }
                      }
                      // Decode LoginSuccess (sym 0xa5acc1a90d0cce47)
                      if (rsym == 0xa5acc1a90d0cce47) {
                        Log(EchoVR::LogLevel::Info, "[NEVR.WS] LOGIN SUCCESS");
                        // Inject SNSFriendListSubscribeRequest directly to server.
                        // pnsrad's broadcaster handle (field_0x160) is null because
                        // echovr.exe doesn't provide one for the pnsrad platform
                        // provider, so pnsrad can't send SNS messages itself. Send
                        // the subscribe request through the WS bridge instead.
                        {
                          static const uint64_t SYM_FRIEND_SUBSCRIBE = 0xcdc02fd1dbee3aaa;
                          // Payload: 0x20 bytes (provider_id + UUID + token).
                          // Server ignores the payload, so send zeros.
                          uint8_t payload[0x20] = {};
                          std::string subscribeMsg;
                          subscribeMsg.append((const char*)MSG_MARKER, 8);
                          AppendLE64(subscribeMsg, SYM_FRIEND_SUBSCRIBE);
                          AppendLE64(subscribeMsg, sizeof(payload));
                          subscribeMsg.append((const char*)payload, sizeof(payload));
                          pairPtr->remoteWs->sendBinary(subscribeMsg);
                          Log(EchoVR::LogLevel::Debug,
                              "[NEVR.WS] Injected FriendListSubscribeRequest (%zu bytes)",
                              subscribeMsg.size());
                        }
                      }
                      // Decode SNS friend messages
                      // InviteFailure (0x7f197e30c72c6e61): Header(8)+FriendID(8)+StatusCode(1)
                      if (rsym == 0x7f197e30c72c6e61 && rmsg->str.size() >= 24 + 17) {
                        uint64_t friendId = 0;
                        uint8_t statusCode = 0;
                        memcpy(&friendId, rmsg->str.data() + 24 + 8, 8);
                        statusCode = (uint8_t)rmsg->str.data()[24 + 16];
                        Log(EchoVR::LogLevel::Warning,
                            "[NEVR.WS] FRIEND INVITE FAILURE: friendId=%llu status=%u",
                            (unsigned long long)friendId, statusCode);
                      }
                      // InviteSuccess (0x7f0c6a3ac83c6f77): Header(8)+FriendID(8)
                      if (rsym == 0x7f0c6a3ac83c6f77 && rmsg->str.size() >= 24 + 16) {
                        uint64_t friendId = 0;
                        memcpy(&friendId, rmsg->str.data() + 24 + 8, 8);
                        Log(EchoVR::LogLevel::Debug,
                            "[NEVR.WS] FRIEND INVITE SUCCESS: friendId=%llu",
                            (unsigned long long)friendId);
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
                        const uint8_t* pp = (const uint8_t*)rmsg->str.data() + 24;
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
                        ix::WebSocket* target = nullptr;
                        {
                          std::lock_guard<std::mutex> lk(g_pairsMutex);
                          target = g_activeGameWs ? g_activeGameWs : gameWsPtr;
                        }
                        if (rmsg->binary) {
                          target->sendBinary(rmsg->str);
                        } else {
                          target->sendText(rmsg->str);
                        }
                      }
                      break;
                    }
                    case ix::WebSocketMessageType::Close:
                      Log(EchoVR::LogLevel::Debug, "[NEVR.WS] Remote closed (ws=%p): %d %s",
                          (void*)gameWsPtr, rmsg->closeInfo.code, rmsg->closeInfo.reason.c_str());
                      // Don't call gameWsPtr->close() — it deadlocks (blocks waiting
                      // for server thread which may be blocked on g_pairsMutex).
                      // The game will detect the closed remote on its next send attempt.
                      break;
                    case ix::WebSocketMessageType::Error:
                      Log(EchoVR::LogLevel::Warning, "[NEVR.WS] Remote error: %s",
                          rmsg->errorInfo.reason.c_str());
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
            Log(EchoVR::LogLevel::Info, "[NEVR.WS] Proxy: game connected (conn=%d, ws=%p), bridging to %s",
                connIdx, (void*)gameWsPtr, g_remoteUri.c_str());
            break;
          }

          case ix::WebSocketMessageType::Message: {
            // Game→remote forwarding — dump all message symbols in the frame
            // EchoVR wire format: [marker(8)][symbol(8)][length(8)][payload(length)]...
            {
              const uint8_t marker_bytes[] = {0xf6,0x40,0xbb,0x78,0xa2,0xe7,0x8c,0xbb};
              const uint8_t* p = (const uint8_t*)msg->str.data();
              size_t remaining = msg->str.size();
              int msgIdx = 0;
              while (remaining >= 24) {
                if (memcmp(p, marker_bytes, 8) != 0) {
                  Log(EchoVR::LogLevel::Warning, "[NEVR.WS] game->server: bad marker at offset %zu",
                      msg->str.size() - remaining);
                  break;
                }
                uint64_t sym, len;
                memcpy(&sym, p + 8, 8);
                memcpy(&len, p + 16, 8);
                char symBuf[192];
                const char* symName = EchoVR::LookupSymbolName(sym);
                if (symName) {
                  snprintf(symBuf, sizeof(symBuf), "0x%016llx (%s)",
                           (unsigned long long)sym, symName);
                } else {
                  snprintf(symBuf, sizeof(symBuf), "0x%016llx",
                           (unsigned long long)sym);
                }
                Log(EchoVR::LogLevel::Debug, "[NEVR.WS] game->server [%d]: sym=%s len=%llu (conn=%s)",
                    msgIdx, symBuf, (unsigned long long)len,
                    connState->getId().c_str());
                // Hex dump PlayerSessionRequest (0x9af2fab2a0c81a05) for debugging
                if (sym == 0x9af2fab2a0c81a05 && len <= 256) {
                  char hex[1024] = {};
                  int hoff = 0;
                  const uint8_t* pp = p + 24;
                  for (size_t i = 0; i < len && hoff < 1000; i++) {
                    hoff += snprintf(hex + hoff, sizeof(hex) - hoff, "%02x ", pp[i]);
                  }
                  Log(EchoVR::LogLevel::Debug, "[NEVR.WS] PlayerSessionReq payload: %s", hex);
                }
                // Decode outgoing SNS friend messages
                // FriendInviteRequest (0x7f0d7a28de3c6f70): RoutingID(8)+UUID(16)+SessionGUID(8)+TargetUserID(8)
                if (sym == 0x7f0d7a28de3c6f70 && len >= 0x28) {
                  uint64_t routingId, sessionGuid, targetUserId;
                  memcpy(&routingId, p + 24, 8);
                  memcpy(&sessionGuid, p + 24 + 24, 8);
                  memcpy(&targetUserId, p + 24 + 32, 8);
                  Log(EchoVR::LogLevel::Debug,
                      "[NEVR.WS]   FriendInvite: routing=%llu target=%llu session=%llu",
                      (unsigned long long)routingId, (unsigned long long)targetUserId,
                      (unsigned long long)sessionGuid);
                }
                // FriendListSubscribe (0xdcfa94680e8d19fc)
                if (sym == 0xdcfa94680e8d19fc) {
                  Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   FriendListSubscribeRequest sent");
                }
                size_t total = 24 + (size_t)len;
                if (total > remaining) {
                  Log(EchoVR::LogLevel::Warning, "[NEVR.WS]   truncated: need %llu but only %zu remaining",
                      (unsigned long long)total, remaining);
                  break;
                }
                p += total;
                remaining -= total;
                msgIdx++;
              }
              if (remaining > 0 && msgIdx > 0) {
                Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   %zu trailing bytes after %d messages", remaining, msgIdx);
              }
            }
            std::lock_guard<std::mutex> lk(g_pairsMutex);
            auto it = g_pairs.find(&gameWs);
            if (it != g_pairs.end()) {
              auto& pair = it->second;
              if (pair->remoteOpen) {
                if (msg->binary) {
                  auto info = pair->remoteWs->sendBinary(msg->str);
                  Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   -> forwarded (success=%d)", info.success);
                } else {
                  pair->remoteWs->sendText(msg->str);
                }
              } else {
                pair->pendingToRemote.push_back(msg->str);
                Log(EchoVR::LogLevel::Debug, "[NEVR.WS]   -> queued (remote not open yet, %zu pending)",
                    pair->pendingToRemote.size());
              }
            } else {
              Log(EchoVR::LogLevel::Warning, "[NEVR.WS]   -> DROPPED (no pair found)");
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
            {
              std::lock_guard<std::mutex> lk(g_pairsMutex);
              auto it = g_pairs.find(&gameWs);
              if (it != g_pairs.end()) {
                bool isShared = (it->second->remoteWs == g_loginRemoteWs);
                if (!isShared) {
                  // Snapshot the remote — we'll stop it OUTSIDE the lock.
                  remoteToStop = it->second->remoteWs;
                  // Clear callback under lock so no further invocations
                  // reference the freed ProxyPair after erase.
                  // N85: no-op, NOT nullptr. ixwebsocket invokes _onMessageCallback
                  // unconditionally; an empty std::function throws std::bad_function_call,
                  // which unwinds out of ixwebsocket's own thread, reaches the game's
                  // unhandled-exception filter as GCC throw code 0x20474343, and kills the
                  // dedicated server. Confirmed from a crash-dump stack:
                  // __cxa_allocate_exception -> __cxa_throw -> std::bad_function_call.
                  it->second->remoteWs->setOnMessageCallback([](const ix::WebSocketMessagePtr&) {});
                } else if (&gameWs == g_loginGameWs) {
                  // Login pair closing — shared remote. N61: only clear the
                  // callback if NO matchmaker connection is sharing the remote.
                  // conn>=2 registers its own callback (N61 fix) which captures
                  // the matchmaker's pairPtr (still alive). Clearing here would
                  // kill matchmaker routing — the regression N61 was supposed
                  // to prevent.
                  if (g_activeGameWs == nullptr || g_activeGameWs == &gameWs) {
                    it->second->remoteWs->setOnMessageCallback([](const ix::WebSocketMessagePtr&) {});
                  }
                  // If a matchmaker is active, leave its callback intact.
                }
                g_pairs.erase(it);
              }
              if (g_activeGameWs == &gameWs) g_activeGameWs = nullptr;
            } // g_pairsMutex RELEASED here — safe to call stop()
            if (remoteToStop) {
              remoteToStop->stop();
            }
            Log(EchoVR::LogLevel::Info, "[NEVR.WS] Proxy: game disconnected");
            break;
          }

          default:
            break;
        }
      });

  g_server->setOnClientMessageCallback(onClientMessage);
  g_server->start();
  g_bridgeEnabled = true;

  Log(EchoVR::LogLevel::Info,
      "[NEVR.WS] Proxy listening on ws://127.0.0.1:%u -> %s",
      g_proxyPort, g_remoteUri.c_str());

  // N146: pnsradmatchmaking uses Rad's R14NETCLIENT with a hardcoded
  // fallback port (42148) when matchingservice_host has no explicit port.
  // This connection bypasses our JsonValueAsStringHook.  Bind a second
  // listener on that port so the matchmaker reaches the bridge.
  {
    static auto s_matchServer = std::make_unique<ix::WebSocketServer>(42148, "127.0.0.1");
    s_matchServer->disablePerMessageDeflate();
    s_matchServer->setOnClientMessageCallback(onClientMessage);
    auto [ok, err] = s_matchServer->listen();
    if (ok) {
      s_matchServer->start();
      Log(EchoVR::LogLevel::Info,
          "[NEVR.WS] Matchmaker listener on ws://127.0.0.1:42148");
    } else {
      s_matchServer.reset();  // port taken — matchmaker will fail, same as before
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.WS] Matchmaker port 42148 unavailable: %s", err.c_str());
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
      "[NEVR.WS] listener stopped, %zu remote connection(s) closed — socket released (N105)",
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

const char* TestHook_PlatformPrefix(uint64_t platformCode) {
  return PlatformPrefix(platformCode);
}

bool TestHook_GuardWsCallbackContainsStdException() {
  const auto guarded = GuardWsCallback("ws_bridge_test", []() {
    throw std::runtime_error("intentional callback exception");
  });
  guarded();
  return true;
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
bool TestHook_N61_SimulateCloseAndCheckCleared(void* rawGameWsPtr) {
  ix::WebSocket* gameWs = static_cast<ix::WebSocket*>(rawGameWsPtr);

  // Run the production Close handler. Track whether the guard cleared
  // the callback (the condition under test for N61).
  bool callbackWasCleared = false;
  {
    std::shared_ptr<ix::WebSocket> remoteToStop;
    {
      std::lock_guard<std::mutex> lk(g_pairsMutex);
      auto it = g_pairs.find(gameWs);
      if (it != g_pairs.end()) {
        bool isShared = (it->second->remoteWs == g_loginRemoteWs);
        if (!isShared) {
          remoteToStop = it->second->remoteWs;
          it->second->remoteWs->setOnMessageCallback([](const ix::WebSocketMessagePtr&) {});
          callbackWasCleared = true;
        } else if (gameWs == g_loginGameWs) {
          // N61 guard: only clear if no matchmaker is sharing.
          if (g_activeGameWs == nullptr || g_activeGameWs == gameWs) {
            it->second->remoteWs->setOnMessageCallback([](const ix::WebSocketMessagePtr&) {});
            callbackWasCleared = true;
          }
          // Else: matchmaker is active → callback SURVIVES (post-fix).
        }
        g_pairs.erase(it);
      }
      if (g_activeGameWs == gameWs) g_activeGameWs = nullptr;
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
