#include "ws_bridge.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/echovr_functions.h"
#include "common/logging.h"

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
};

static std::mutex g_pairsMutex;
static std::unordered_map<ix::WebSocket*, std::unique_ptr<ProxyPair>> g_pairs;

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
            auto remote = std::make_shared<ix::WebSocket>();
            remote->setUrl(g_remoteUri);
            remote->disableAutomaticReconnection();
            remote->disablePerMessageDeflate();

            auto pair = std::make_unique<ProxyPair>();
            pair->remoteWs = remote;

            auto* pairPtr = pair.get();
            ix::WebSocket* gameWsPtr = &gameWs;

            // Remote → game forwarding
            remote->setOnMessageCallback(
                [pairPtr, gameWsPtr](const ix::WebSocketMessagePtr& rmsg) {
                  switch (rmsg->type) {
                    case ix::WebSocketMessageType::Open: {
                      std::lock_guard<std::mutex> lk(g_pairsMutex);
                      pairPtr->remoteOpen = true;
                      Log(EchoVR::LogLevel::Info, "[NEVR.WS] Remote open: %s",
                          g_remoteUri.c_str());
                      for (auto& pending : pairPtr->pendingToRemote) {
                        pairPtr->remoteWs->sendBinary(pending);
                      }
                      pairPtr->pendingToRemote.clear();
                      break;
                    }
                    case ix::WebSocketMessageType::Message:
                      // Forward server→game
                      if (rmsg->binary) {
                        gameWsPtr->sendBinary(rmsg->str);
                      } else {
                        gameWsPtr->sendText(rmsg->str);
                      }
                      break;
                    case ix::WebSocketMessageType::Close:
                      Log(EchoVR::LogLevel::Info, "[NEVR.WS] Remote closed: %d %s",
                          rmsg->closeInfo.code, rmsg->closeInfo.reason.c_str());
                      gameWsPtr->close();
                      break;
                    case ix::WebSocketMessageType::Error:
                      Log(EchoVR::LogLevel::Warning, "[NEVR.WS] Remote error: %s",
                          rmsg->errorInfo.reason.c_str());
                      gameWsPtr->close();
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
            Log(EchoVR::LogLevel::Info, "[NEVR.WS] Proxy: game connected, bridging to %s",
                g_remoteUri.c_str());
            break;
          }

          case ix::WebSocketMessageType::Message: {
            // Game→remote forwarding
            std::lock_guard<std::mutex> lk(g_pairsMutex);
            auto it = g_pairs.find(&gameWs);
            if (it != g_pairs.end()) {
              auto& pair = it->second;
              if (pair->remoteOpen) {
                if (msg->binary) {
                  pair->remoteWs->sendBinary(msg->str);
                } else {
                  pair->remoteWs->sendText(msg->str);
                }
              } else {
                pair->pendingToRemote.push_back(msg->str);
              }
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
