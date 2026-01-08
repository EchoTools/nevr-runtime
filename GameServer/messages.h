#pragma once
#ifndef GAMESERVER_MESSAGES_H
#define GAMESERVER_MESSAGES_H

#include "echovr.h"
#include "symbols.h"

// Namespace aliases for convenience
namespace Sym = EchoVR::Symbols;
namespace TcpSym = EchoVR::Symbols::Tcp;

// Message structures

// Registration request from game server to serverdb
struct ERLobbyRegistrationRequest {
  uint64_t serverId;
  uint32_t internalIp;
  uint16_t port;
  uint8_t padding[10];
  EchoVR::SymbolId regionId;
  EchoVR::SymbolId versionLock;
};

// Session end notification
struct ERLobbyEndSession {
  char unused;
};

// Player sessions locked notification
struct ERLobbyPlayerSessionsLocked {
  char unused;
};

// Player sessions unlocked notification
struct ERLobbyPlayerSessionsUnlocked {
  char unused;
};

#endif  // GAMESERVER_MESSAGES_H
