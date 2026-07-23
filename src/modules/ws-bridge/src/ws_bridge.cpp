#include "ws_bridge.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/nevr_module_interface.h"
#include "common/echovr_functions.h"
#include "common/logging.h"

// Stored from NvrModuleContext for cross-module proc resolution
static void* (*s_getProc)(const char*) = nullptr;
static void* s_earlyConfig = nullptr;
static bool g_isServer = false;
static bool g_noOvr = false;

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

static const char* PlatformPrefix(uint64_t platformCode) {
  switch (platformCode) {
    case 1: return "STM";
    case 2: return "DSC";
    case 3: return "XBX";
    case 4: return "OVR-ORG";
    case 5: return "DSC-NOVR";
    default: return "UNK";
  }
}

static std::string BuildLoginRequest(uint64_t discordId, uint64_t platformCode = 2) {
  // Platform: DSC = 2 (Go iota: XPlatformIdSize=0, STM=1, PSN/DSC=2, XBX=3, OVR_ORG=4)
  // PSN=2 slot is reused for DSC by PatchDscProvider (xpid_patch.cpp) which
  // rewrites "PSN"->"DSC" in the game's string tables so all ~70+ inlined
  // format switches produce "DSC-" from provider_id 2.
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
  g_server->setOnClientMessageCallback(
      [](std::shared_ptr<ix::ConnectionState> connState,
         ix::WebSocket& gameWs,
         const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
          case ix::WebSocketMessageType::Open: {
            // Game opened a connection — create remote ws to real server
            int connIdx = g_connectionCount++;
            auto remote = std::make_shared<ix::WebSocket>();

            // Build remote URL with optional query param auth
            // (workaround: production nginx strips Bearer JWT and forces format=evr)
            std::string remoteUrl = g_remoteUri;
            if ((EchoVR::Json*)s_earlyConfig) {
              CHAR* cfgDiscordId = EchoVR::JsonValueAsString((EchoVR::Json*)s_earlyConfig, (CHAR*)"nevr_discord_id", NULL, false);
              CHAR* cfgPassword = EchoVR::JsonValueAsString((EchoVR::Json*)s_earlyConfig, (CHAR*)"nevr_password", NULL, false);
              if (cfgDiscordId && cfgDiscordId[0] != '\0' && cfgPassword && cfgPassword[0] != '\0') {
                char sep = (remoteUrl.find('?') != std::string::npos) ? '&' : '?';
                remoteUrl += sep;
                remoteUrl += "discordid=";
                remoteUrl += cfgDiscordId;
                remoteUrl += "&password=";
                remoteUrl += cfgPassword;
              }
            }
            remote->setUrl(remoteUrl);
            remote->disableAutomaticReconnection();
            remote->disablePerMessageDeflate();

            // Get auth token and Discord ID.
            // In client mode, the identity comes from the token_auth module's JWT.
            // In server mode, token_auth is disabled so the JWT may be absent — fall
            // back to nevr_discord_id from config (server identity is static).
            std::string bearerToken;
            uint64_t discordId = 0;
            {
              auto getTokenFn = (const char* (*)())s_getProc("TokenAuth_GetToken");
              auto getDiscordIdFn = (uint64_t (*)())s_getProc("TokenAuth_GetDiscordId");
              if (getTokenFn) {
                const char* tok = getTokenFn();
                if (tok) bearerToken = tok;
              }
              if (getDiscordIdFn) discordId = getDiscordIdFn();
            }
            if (discordId == 0 && g_isServer && (EchoVR::Json*)s_earlyConfig) {
              CHAR* cfgId = EchoVR::JsonValueAsString(
                  (EchoVR::Json*)s_earlyConfig, (CHAR*)"nevr_discord_id", NULL, false);
              if (cfgId && cfgId[0] != '\0') {
                discordId = strtoull(cfgId, nullptr, 10);
                Log(EchoVR::LogLevel::Debug,
                    "[NEVR.WS] Using nevr_discord_id from config: %llu",
                    (unsigned long long)discordId);
              }
            }
            if (discordId == 0) {
              Log(EchoVR::LogLevel::Warning,
                  "[NEVR.WS] No discord ID available — LoginRequest will use account ID 0");
            }
            if (!bearerToken.empty()) {
              ix::WebSocketHttpHeaders headers;
              headers["Authorization"] = "Bearer " + bearerToken;
              remote->setExtraHeaders(headers);
              Log(EchoVR::LogLevel::Debug, "[NEVR.WS] Attaching Bearer token to remote connection");
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
                      if (!pairPtr->loginInjected) {
                        pairPtr->loginInjected = true;

                        // Set CNSUser login state via pnsrad.dll's Users() singleton
                        HMODULE hPnsrad = GetModuleHandleA("pnsrad.dll");
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
                                Log(EchoVR::LogLevel::Debug,
                                    "[NEVR.WS] CNSUser BEFORE: state=0x%llx flags=0x%x",
                                    (unsigned long long)*loginState, *stateFlags);
                                // Set login state to kLoggingIn (2)
                                *loginState = (*loginState & ~0xFULL) | 2;
                                // Clear all flags, set only connecting (bit 2)
                                *stateFlags = 0x04;
                                Log(EchoVR::LogLevel::Debug,
                                    "[NEVR.WS] CNSUser AFTER:  state=0x%llx flags=0x%x",
                                    (unsigned long long)*loginState, *stateFlags);
                              }
                            }
                          }
                        }

                        uint64_t platformCode = g_noOvr ? static_cast<uint64_t>(5) : static_cast<uint64_t>(2);
                        std::string loginMsg = BuildLoginRequest(discordId, platformCode);
                        pairPtr->remoteWs->sendBinary(loginMsg);
                        std::string xpid = std::string(PlatformPrefix(platformCode)) + "-" + std::to_string(discordId);
                        Log(EchoVR::LogLevel::Debug,
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
                      Log(EchoVR::LogLevel::Debug, "[NEVR.WS] server->game [conn=%d]: %zu bytes sym=0x%016llx payloadLen=%llu",
                          connIdx, rmsg->str.size(), (unsigned long long)rsym, (unsigned long long)rlen);
                      // Decode LoginFailure error message (sym 0xa5b9d5a3021ccf51)
                      if (rsym == 0xa5b9d5a3021ccf51 && rmsg->str.size() > 48) {
                        uint64_t statusCode = 0;
                        memcpy(&statusCode, rmsg->str.data() + 24 + 16, 8);
                        const char* errMsg = rmsg->str.data() + 24 + 24;
                        size_t errMaxLen = rmsg->str.size() - 48;
                        Log(EchoVR::LogLevel::Warning, "[NEVR.WS] LOGIN FAILURE: status=%llu msg=%.*s",
                            (unsigned long long)statusCode, (int)errMaxLen, errMsg);

                        // Server mode: fake a LoginSuccess to advance past login gate.
                        // The GameServerLib handles its own auth to ServerDB separately.
                        // connIdx >= 0 covers both the config connection (0) and
                        // explicit login connections (>0).
                        if (g_isServer && connIdx >= 0) {
                          Log(EchoVR::LogLevel::Debug,
                              "[NEVR.WS] Server mode — injecting fake LoginSuccess to bypass login gate");
                          static const uint64_t SYM_LOGIN_SUCCESS = 0xa5acc1a90d0cce47;
                          // LoginSuccess payload: Session UUID(16) + PlatformCode(8) + AccountId(8)
                          uint8_t payload[32] = {};
                          // Generate a dummy session UUID (non-zero)
                          payload[0] = 0x4E; payload[1] = 0x45; payload[2] = 0x56; payload[3] = 0x52; // "NEVR"
                          payload[4] = 0x53; payload[5] = 0x52; payload[6] = 0x56; payload[7] = 0x52; // "SRVR"
                          uint64_t platformCode = g_noOvr ? static_cast<uint64_t>(5) : static_cast<uint64_t>(2);
                          memcpy(payload + 16, &platformCode, 8);
                          // AccountId: the server's Discord ID
                          memcpy(payload + 24, &discordId, 8);

                          std::string fakeSuccess;
                          fakeSuccess.append((const char*)MSG_MARKER, 8);
                          AppendLE64(fakeSuccess, SYM_LOGIN_SUCCESS);
                          AppendLE64(fakeSuccess, sizeof(payload));
                          fakeSuccess.append((const char*)payload, sizeof(payload));
                          gameWsPtr->sendBinary(fakeSuccess);

                          // Don't forward the failure to the game
                          break;
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
                      // FriendListResponse (0xa78aeb2a4e89b10b): counts
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
                      }
                      // Inject LoginRequest on config connection (conn=0) after first
                      // server→game message (config data).  The server at /spr sends
                      // config then waits for a login request; if none arrives within
                      // ~5s it closes the connection.  Injecting here ensures login
                      // proceeds even if the game never opens a second connection for
                      // login_host (e.g. because the server config overwrote the URL
                      // with a non-readyatdawn.com value before the redirect could fire).
                      if (!pairPtr->loginInjected && pairPtr->remoteOpen) {
                        pairPtr->loginInjected = true;

                        // Set CNSUser login state via pnsrad.dll's Users() singleton.
                        // On the config connection the user may not exist yet; if so,
                        // the LoginRequest is still sent — the LoginFailure handler
                        // above will inject a fake LoginSuccess in server mode.
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
                                uint64_t* loginState = (uint64_t*)(user + 0x90);
                                uint32_t* stateFlags = (uint32_t*)(user + 0x9c);
                                Log(EchoVR::LogLevel::Debug,
                                    "[NEVR.WS] CNSUser BEFORE: state=0x%llx flags=0x%x",
                                    (unsigned long long)*loginState, *stateFlags);
                                *loginState = (*loginState & ~0xFULL) | 2;
                                *stateFlags = 0x04;
                                Log(EchoVR::LogLevel::Debug,
                                    "[NEVR.WS] CNSUser AFTER:  state=0x%llx flags=0x%x",
                                    (unsigned long long)*loginState, *stateFlags);
                              } else {
                                Log(EchoVR::LogLevel::Debug,
                                    "[NEVR.WS] CNSUser not yet created (count=%llu) — "
                                    "LoginRequest sent without state prep; LoginFailure handler will inject fake success",
                                    (unsigned long long)userCount);
                              }
                            }
                          }
                        }

                        uint64_t platformCode = g_noOvr ? static_cast<uint64_t>(5) : static_cast<uint64_t>(2);
                        std::string loginMsg = BuildLoginRequest(discordId, platformCode);
                        pairPtr->remoteWs->sendBinary(loginMsg);
                        std::string xpid = std::string(PlatformPrefix(platformCode)) + "-" + std::to_string(discordId);
                        Log(EchoVR::LogLevel::Debug,
                            "[NEVR.WS] login injected xpid=%s platform=%d conn=%d size=%zu",
                            xpid.c_str(), static_cast<int>(platformCode), connIdx, loginMsg.size());
                      }
                      if (rmsg->binary) {
                        gameWsPtr->sendBinary(rmsg->str);
                      } else {
                        gameWsPtr->sendText(rmsg->str);
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
                });

            {
              std::lock_guard<std::mutex> lk(g_pairsMutex);
              g_pairs[gameWsPtr] = std::move(pair);
            }
            // Start after insertion so the remote callback can find the pair in g_pairs
            remote->start();
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
                Log(EchoVR::LogLevel::Debug, "[NEVR.WS] game->server [%d]: sym=0x%016llx len=%llu (conn=%s)",
                    msgIdx, (unsigned long long)sym, (unsigned long long)len,
                    connState->getId().c_str());
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

  g_server->start();
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

// Graceful shutdown — called from gamepatches when NOT under the loader lock
// (CTRL+C signal handler, session-end teardown, etc.). Actually stops the
// listener and remote connections, releasing the socket FD so the wineserver
// doesn't leak it as a zombie LISTEN socket.
NEVR_MODULE_API void WsBridge_Shutdown(void) {
  fprintf(stderr, "[NEVR.WS] WsBridge_Shutdown: stopping listener and remote connections\n");
  fflush(stderr);
  g_bridgeEnabled = false;

  // Stop all remote connections. Extract shared_ptrs under the lock,
  // then stop outside to avoid deadlock with callback threads.
  {
    std::vector<std::shared_ptr<ix::WebSocket>> remotes;
    {
      std::lock_guard<std::mutex> lk(g_pairsMutex);
      for (auto& pair : g_pairs) {
        if (pair.second && pair.second->remoteWs) {
          remotes.push_back(pair.second->remoteWs);
        }
      }
      g_pairs.clear();
    }
    for (auto& ws : remotes) {
      ws->stop();
    }
    fprintf(stderr, "[NEVR.WS] Stopped %zu remote connections\n", remotes.size());
    fflush(stderr);
  }

  // Stop the listener — this is the critical step that releases the socket FD
  // and prevents the wineserver zombie on Linux/Wine.
  if (g_server) {
    g_server->stop();
    g_server.reset();
    fprintf(stderr, "[NEVR.WS] Listener stopped — socket released\n");
    fflush(stderr);
  }
}

// ---------------------------------------------------------------------------
// Module interface
// ---------------------------------------------------------------------------

NEVR_MODULE_API int NvrModuleInit(const NvrModuleContext* ctx) {
  EchoVR::g_GameBaseAddress = (CHAR*)ctx->base_addr;
  EchoVR::InitializeFunctionPointers();
  s_getProc = ctx->get_proc;
  s_earlyConfig = ctx->early_config;
  g_isServer = (ctx->flags & NEVR_MODULE_HOST_IS_SERVER) != 0;
  g_noOvr = (ctx->flags & NEVR_MODULE_HOST_IS_NOOVR) != 0;

  // Read socket URI from config
  if (s_earlyConfig) {
    CHAR* socketUri = EchoVR::JsonValueAsString(
        (EchoVR::Json*)s_earlyConfig, (CHAR*)"nevr_socket_uri", NULL, false);
    if (socketUri) {
      SetWebSocketBridgeTarget(socketUri);
      InstallWebSocketBridge();
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.WS] No nevr_socket_uri in config — bridge disabled");
    }
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.WS] No early config — bridge disabled");
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.MODULE] ws_bridge initialized");
  return 0;
}

NEVR_MODULE_API void NvrModuleShutdown(void) {
  ShutdownWebSocketBridge();
}

// C exports for cross-module resolution (config.cpp URL redirect uses these)
NEVR_MODULE_API uint16_t WsBridge_GetPort(void) {
  return GetWebSocketBridgePort();
}

NEVR_MODULE_API bool WsBridge_IsActive(void) {
  return IsWebSocketBridgeActive();
}
