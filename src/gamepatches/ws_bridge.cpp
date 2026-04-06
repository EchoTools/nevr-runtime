#include "ws_bridge.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/echovr_functions.h"
#include "common/logging.h"
#include "common/auth_token.h"

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

static std::string BuildLoginRequest(uint64_t discordId) {
  // Platform: OVR_ORG = 4 (Go iota: XPlatformIdSize=0, STM=1, PSN=2, XBX=3, OVR_ORG=4)
  uint64_t platformCode = 4;
  uint64_t accountId = discordId;

  // LoginProfile JSON — matches the game's SNSLogInRequestv2 format
  char json[2048];
  snprintf(json, sizeof(json),
    "{"
      "\"accountid\":%llu,"
      "\"displayname\":\"nEVR\","
      "\"bypassauth\":false,"
      "\"access_token\":\"\","
      "\"nonce\":\"\","
      "\"buildversion\":631547,"
      "\"lobbyversion\":0,"
      "\"appid\":0,"
      "\"publisher_lock\":\"\","
      "\"hmdserialnumber\":\"nEVR-Wine\","
      "\"desiredclientprofileversion\":0,"
      "\"system_info\":{"
        "\"headset_type\":\"No VR\","
        "\"driver_version\":\"\","
        "\"network_type\":\"WireGuard\","
        "\"video_card\":\"Wine D3D12\","
        "\"cpu\":\"Wine\","
        "\"num_physical_cores\":4,"
        "\"num_logical_cores\":8,"
        "\"memory_total\":16384,"
        "\"memory_used\":8192,"
        "\"dedicated_gpu_memory\":8192"
      "}"
    "}",
    (unsigned long long)accountId);

  size_t jsonLen = strlen(json) + 1;  // include null terminator

  // Build payload: UUID(16) + PlatformCode(8) + AccountId(8) + JSON+null
  std::string payload;
  payload.reserve(16 + 8 + 8 + jsonLen);
  // UUID = all zeros (no previous session)
  for (int i = 0; i < 16; i++) payload.push_back('\0');
  AppendLE64(payload, platformCode);
  AppendLE64(payload, accountId);
  payload.append(json, jsonLen);

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

  // Start local ws:// server on a dynamic port
  g_server = std::make_unique<ix::WebSocketServer>(6821, "127.0.0.1");
  g_server->disablePerMessageDeflate();

  g_server->setOnClientMessageCallback(
      [](std::shared_ptr<ix::ConnectionState> connState,
         ix::WebSocket& gameWs,
         const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
          case ix::WebSocketMessageType::Open: {
            // Game opened a connection — create remote ws to real server
            int connIdx = g_connectionCount++;
            auto remote = std::make_shared<ix::WebSocket>();
            remote->setUrl(g_remoteUri);
            remote->disableAutomaticReconnection();
            remote->disablePerMessageDeflate();

            // Attach Bearer token if we have cached credentials
            auto cachedAuth = LoadCachedAuthToken();
            uint64_t discordId = 695081603180789771ULL;  // TODO: read from JWT `did` claim
            if (cachedAuth.HasValidToken()) {
              ix::WebSocketHttpHeaders headers;
              headers["Authorization"] = "Bearer " + cachedAuth.token;
              remote->setExtraHeaders(headers);
              Log(EchoVR::LogLevel::Info, "[NEVR.WS] Attaching Bearer token to remote connection");
            }

            auto pair = std::make_unique<ProxyPair>();
            pair->remoteWs = remote;

            auto* pairPtr = pair.get();
            ix::WebSocket* gameWsPtr = &gameWs;

            // Remote → game forwarding
            remote->setOnMessageCallback(
                [pairPtr, gameWsPtr, connIdx, discordId](const ix::WebSocketMessagePtr& rmsg) {
                  switch (rmsg->type) {
                    case ix::WebSocketMessageType::Open: {
                      std::lock_guard<std::mutex> lk(g_pairsMutex);
                      pairPtr->remoteOpen = true;
                      Log(EchoVR::LogLevel::Info, "[NEVR.WS] Remote open (conn=%d): %s",
                          connIdx, g_remoteUri.c_str());

                      // Inject LoginRequest on login connections (not config).
                      // pnsrad.dll won't send its own because it has no user identity
                      // (OVR SDK is bypassed). The LoginRequest is built and injected here.
                      //
                      // Before injecting, set the CNSUser's login state to "logging in"
                      // so that CNSUser::LogInSuccessCB processes the server's LoginSuccess
                      // response. Without this, LogInSuccessCB silently discards the message
                      // because the user's login state at +0x90 is still 0 (logged out).
                      if (connIdx > 0 && !pairPtr->loginInjected) {
                        pairPtr->loginInjected = true;

                        // Set CNSUser login state via pnsrad.dll's Users() singleton
                        HMODULE hPnsrad = GetModuleHandleA("pnsrad");
                        if (hPnsrad) {
                          typedef void* (*UsersFn)();
                          auto Users = (UsersFn)GetProcAddress(hPnsrad, "Users");
                          if (Users) {
                            auto* usersObj = (uint8_t*)Users();
                            if (usersObj) {
                              // CNSIUsers layout: +0x368 = buffer_ctx (pointer to first user)
                              // +0x398 = active_user_count
                              uint64_t userCount = *(uint64_t*)(usersObj + 0x398);
                              uint8_t** bufCtx = *(uint8_t***)(usersObj + 0x368);
                              if (userCount > 0 && bufCtx && *bufCtx) {
                                uint8_t* user = *bufCtx;
                                // CNSUser +0x90 = login state (low nibble: 0=out, 2=logging in, 6=in)
                                // CNSUser +0x9c = state flags (bit 2=connecting, bit 4=offline)
                                uint64_t* loginState = (uint64_t*)(user + 0x90);
                                uint32_t* stateFlags = (uint32_t*)(user + 0x9c);
                                Log(EchoVR::LogLevel::Info,
                                    "[NEVR.WS] CNSUser BEFORE: state=0x%llx flags=0x%x",
                                    (unsigned long long)*loginState, *stateFlags);
                                // Set login state to kLoggingIn (2)
                                *loginState = (*loginState & ~0xFULL) | 2;
                                // Clear all flags, set only connecting (bit 2)
                                *stateFlags = 0x04;
                                Log(EchoVR::LogLevel::Info,
                                    "[NEVR.WS] CNSUser AFTER:  state=0x%llx flags=0x%x",
                                    (unsigned long long)*loginState, *stateFlags);
                              }
                            }
                          }
                        }

                        std::string loginMsg = BuildLoginRequest(discordId);
                        pairPtr->remoteWs->sendBinary(loginMsg);
                        Log(EchoVR::LogLevel::Info,
                            "[NEVR.WS] Injected LoginRequest (OVR-ORG-%llu, %zu bytes)",
                            (unsigned long long)discordId, loginMsg.size());
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
                      Log(EchoVR::LogLevel::Info, "[NEVR.WS] server->game: %zu bytes sym=0x%016llx payloadLen=%llu",
                          rmsg->str.size(), (unsigned long long)rsym, (unsigned long long)rlen);
                      // Decode LoginFailure error message (sym 0xa5b9d5a3021ccf51)
                      if (rsym == 0xa5b9d5a3021ccf51 && rmsg->str.size() > 48) {
                        // payload: PlatformCode(8) + AccountId(8) + StatusCode(8) + ErrorMsg\0
                        uint64_t statusCode = 0;
                        memcpy(&statusCode, rmsg->str.data() + 24 + 16, 8);
                        const char* errMsg = rmsg->str.data() + 24 + 24;
                        size_t errMaxLen = rmsg->str.size() - 48;
                        Log(EchoVR::LogLevel::Warning, "[NEVR.WS] LOGIN FAILURE: status=%llu msg=%.*s",
                            (unsigned long long)statusCode, (int)errMaxLen, errMsg);
                      }
                      // Decode LoginSuccess (sym 0xa5acc1a90d0cce47)
                      if (rsym == 0xa5acc1a90d0cce47) {
                        Log(EchoVR::LogLevel::Info, "[NEVR.WS] LOGIN SUCCESS");
                      }
                      if (rmsg->binary) {
                        gameWsPtr->sendBinary(rmsg->str);
                      } else {
                        gameWsPtr->sendText(rmsg->str);
                      }
                      break;
                    }
                    case ix::WebSocketMessageType::Close:
                      Log(EchoVR::LogLevel::Info, "[NEVR.WS] Remote closed (ws=%p): %d %s",
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
                });

            remote->start();

            {
              std::lock_guard<std::mutex> lk(g_pairsMutex);
              g_pairs[gameWsPtr] = std::move(pair);
            }
            Log(EchoVR::LogLevel::Info, "[NEVR.WS] Proxy: game connected (conn=%s, ws=%p), bridging to %s",
                connState->getId().c_str(), (void*)gameWsPtr, g_remoteUri.c_str());
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
                Log(EchoVR::LogLevel::Info, "[NEVR.WS] game->server [%d]: sym=0x%016llx len=%llu (conn=%s)",
                    msgIdx, (unsigned long long)sym, (unsigned long long)len,
                    connState->getId().c_str());
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
                Log(EchoVR::LogLevel::Info, "[NEVR.WS]   %zu trailing bytes after %d messages", remaining, msgIdx);
              }
            }
            std::lock_guard<std::mutex> lk(g_pairsMutex);
            auto it = g_pairs.find(&gameWs);
            if (it != g_pairs.end()) {
              auto& pair = it->second;
              if (pair->remoteOpen) {
                if (msg->binary) {
                  auto info = pair->remoteWs->sendBinary(msg->str);
                  Log(EchoVR::LogLevel::Info, "[NEVR.WS]   -> forwarded (success=%d)", info.success);
                } else {
                  pair->remoteWs->sendText(msg->str);
                }
              } else {
                pair->pendingToRemote.push_back(msg->str);
                Log(EchoVR::LogLevel::Info, "[NEVR.WS]   -> queued (remote not open yet, %zu pending)",
                    pair->pendingToRemote.size());
              }
            } else {
              Log(EchoVR::LogLevel::Warning, "[NEVR.WS]   -> DROPPED (no pair found)");
            }
            break;
          }

          case ix::WebSocketMessageType::Close: {
            std::lock_guard<std::mutex> lk(g_pairsMutex);
            auto it = g_pairs.find(&gameWs);
            if (it != g_pairs.end()) {
              it->second->remoteWs->stop();
              g_pairs.erase(it);
            }
            Log(EchoVR::LogLevel::Info, "[NEVR.WS] Proxy: game disconnected");
            break;
          }

          default:
            break;
        }
      });

  auto [ok, errMsg] = g_server->listen();
  if (!ok) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.WS] Failed to listen: %s", errMsg.c_str());
    g_server.reset();
    return;
  }

  g_server->start();
  g_proxyPort = g_server->getPort();
  g_bridgeEnabled = true;

  Log(EchoVR::LogLevel::Info,
      "[NEVR.WS] Proxy listening on ws://127.0.0.1:%u -> %s",
      g_proxyPort, g_remoteUri.c_str());
}

void ShutdownWebSocketBridge() {
  g_bridgeEnabled = false;
  // Do NOT call g_server->stop() — runs under loader lock during DLL_PROCESS_DETACH.
  // Thread joins can deadlock. OS reclaims everything on process exit.
}
