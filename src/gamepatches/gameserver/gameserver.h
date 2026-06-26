#pragma once

#include <atomic>
#include <memory>

#include "constants.h"
#include "echovr.h"
#include "pch.h"
#include "server_context.h"
#include "telemetry_streamer.h"
#include "websocket_client.h"

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
  /// send EndSession, call Unregister, then ExitProcess(0).
  /// @param registrationFailed  true if we're shutting down because registration was rejected.
  void BeginGracefulShutdown(bool registrationFailed);

 private:
  std::unique_ptr<GameServer::ServerContext> m_context;
  std::unique_ptr<WebSocketClient> m_wsClient;
  std::unique_ptr<TelemetryStreamer> m_telemetry;

  // Set by the detached graceful-shutdown thread when it finishes.
  // Destructor waits on this before tearing down members.
  std::atomic<bool> m_shutdownComplete{false};

  // Helper methods
  void RegisterBroadcasterCallbacks();
  void RegisterTcpCallbacks();
  void UnregisterAllCallbacks();
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
