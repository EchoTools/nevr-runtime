#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "runtime/server/constants.h"
#include "abi/echovr.h"
#include "core/pch.h"
#include "runtime/server/main_thread_handoff.h"
#include "runtime/server/server_context.h"
#include "runtime/server/telemetry_streamer.h"
#include "runtime/server/websocket_client.h"

// IServerLib implementation connecting to NEVR's ServerDB service.
// Manages game server registration, sessions, and player lifecycle.
class GameServerLib : public EchoVR::IServerLib {
 public:
  GameServerLib();
  ~GameServerLib();

  // IServerLib interface (vtable order matters)
  INT64 UnkFunc0(VOID* unk1, INT64 a2, INT64 a3) override;
  VOID* Initialize(EchoVR::Lobby* lobby, EchoVR::Broadcaster* broadcaster, VOID* unk2, const CHAR* logPath) override;
  VOID Terminate() override;
  VOID Update() override;
  VOID UnkFunc1(UINT64 unk) override;

  VOID RequestRegistration(INT64 serverId, CHAR* radId, EchoVR::SymbolId regionId, EchoVR::SymbolId lockedVersion,
                           const EchoVR::Json* localConfig) override;
  VOID Unregister() override;
  VOID EndSession() override;
  VOID LockPlayerSessions() override;
  VOID UnlockPlayerSessions() override;
  VOID AcceptPlayerSessions(EchoVR::Array<GUID>* playerUuids) override;
  VOID RemovePlayerSession(GUID* playerUuid) override;

  // Context accessor for callback handlers
  GameServer::ServerContext& GetContext() { return *m_context; }
  const GameServer::ServerContext& GetContext() const { return *m_context; }

  // WebSocketClient accessor for SendProtobufEnvelope
  WebSocketClient& GetWsClient() { return *m_wsClient; }

  // TelemetryStreamer accessor
  TelemetryStreamer& GetTelemetry() { return *m_telemetry; }

  /// Initiate graceful shutdown: disable reconnection, wait for round end (if active),
  /// have the game thread run EndSession + Unregister (GH #44), then exit via
  /// ForceFatalExit(0).
  /// @param registrationFailed  true if we're shutting down because registration was rejected.
  void BeginGracefulShutdown(bool registrationFailed);

 private:
  std::unique_ptr<GameServer::ServerContext> m_context;
  std::unique_ptr<WebSocketClient> m_wsClient;
  std::unique_ptr<TelemetryStreamer> m_telemetry;

  // Set by the graceful-shutdown thread when it finishes.
  // Destructor waits on this before tearing down members.
  std::atomic<bool> m_shutdownComplete{false};

  // GH #44: the shutdown thread hands EndSession + Unregister to the game thread
  // through this; Update() services it. Declared before m_shutdownThread so it
  // outlives the thread that waits on it.
  GameServer::MainThreadHandoff m_gameThreadHandoff;

  // Thread that registered the broadcaster callbacks (the game thread). The
  // callback registry is only safe on that thread; UnregisterAllCallbacks logs
  // a warning when it is reached from any other.
  std::atomic<DWORD> m_registryThreadId{0};

  // Joinable handle for the shutdown thread — replaces detached thread.
  // Joined in ~GameServerLib to prevent use-after-free on member destruction.
  std::thread m_shutdownThread;

  // Helper methods
  void RegisterBroadcasterCallbacks();
  void RegisterTcpCallbacks();
  void UnregisterAllCallbacks();

  // Connects the telemetry streamer when telemetry is enabled and configured (gameserver_telemetry.cpp);
  // `wsToken` is the ServerDB token, used when no telemetry_token is configured.
  void ConnectTelemetry(const std::string& wsToken);

  // Unregister() body. touchCallbackRegistry=false skips UnregisterAllCallbacks
  // and is the only form allowed off the game thread.
  void UnregisterFromServerDb(bool touchCallbackRegistry);

  // Shutdown-thread work, split by the thread it may run on (GH #44).
  void ShutdownUnregisterOnGameThread();   // EndSession + full Unregister
  void ShutdownUnregisterOffGameThread();  // EndSession + ServerDB unregister, registry untouched
};

// Logging helper (uses game's logging system)
void Log(EchoVR::LogLevel level, const CHAR* format, ...);

// Callback registration helpers
uint16_t ListenForBroadcasterMessage(GameServerLib* self, EchoVR::SymbolId msgId, BOOL isMsgReliable, VOID* func);

// Slot index extraction from message payloads
struct SlotInfo {
  uint16_t slot;
  uint16_t genId;
};
SlotInfo ExtractSlotIndex(const void* msg, uint64_t msgSize);
