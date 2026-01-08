#pragma once
#ifndef GAMESERVER_H
#define GAMESERVER_H

#include <memory>

#include "constants.h"
#include "echovr.h"
#include "pch.h"
#include "servercontext.h"

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
  GameServer::ServerContext& GetContext() { return *context_; }
  const GameServer::ServerContext& GetContext() const { return *context_; }

 private:
  std::unique_ptr<GameServer::ServerContext> context_;

  // Helper methods
  void RegisterBroadcasterCallbacks();
  void RegisterTcpCallbacks();
  void UnregisterAllCallbacks();
};

// Logging helper (uses game's logging system)
void Log(EchoVR::LogLevel level, const CHAR* format, ...);

// Callback registration helpers
uint16_t ListenForBroadcasterMessage(GameServerLib* self, EchoVR::SymbolId msgId, BOOL isMsgReliable, VOID* func);
uint16_t ListenForTcpBroadcasterMessage(GameServerLib* self, EchoVR::SymbolId msgId, VOID* func);

// Slot index extraction from message payloads
struct SlotInfo {
  uint16_t slot;
  uint16_t genId;
};
SlotInfo ExtractSlotIndex(const void* msg, uint64_t msgSize);

#endif  // GAMESERVER_H
