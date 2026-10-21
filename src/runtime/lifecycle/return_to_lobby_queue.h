#pragma once

// #730: the game's NetGameScheduleReturnToLobby (echovr.exe 0x1401a89f0) queues its callback on the
// game thread's call queue through the net game's game object:
//     queue = *(netGame + 8) + 8;        // NETGAME_GAME_OFFSET, GAME_CALL_QUEUE_OFFSET
//     queue_state_event(queue, &call);   // 0x140104e70 reads [queue] first, then [[queue] + 0x80]
// While the queue's first field is null (a server that sits in its lobby and whose session the
// service ends before a match started), the read of [[queue] + 0x80] faults at game+0x104E93 and
// the server ends in the crash recovery's server hold. This header decides, from the same three
// fields, whether the game's function can be called. Pure: no logging, no calls into the game.

#include <cstdint>
#include <cstring>

#include "runtime/hook/addresses.h"

namespace nevr_return_to_lobby_queue {

enum class Verdict {
  Ready,             // the queue exists: the game's function can be called
  NoNetGame,         // no net game pointer yet
  NoGameObject,      // the net game has no game object
  QueueNotAllocated  // the queue's first field is null: the game's queue push would fault
};

inline const void* ReadPointer(const void* base, uintptr_t offset) {
  const void* value = nullptr;
  std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
  return value;
}

/// `netGame` is the pointer NetGameScheduleReturnToLobby takes (the runtime's g_pGame).
inline Verdict Check(const void* netGame) {
  if (netGame == nullptr) return Verdict::NoNetGame;
  const void* game = ReadPointer(netGame, nevr_patch_addresses::NETGAME_GAME_OFFSET);
  if (game == nullptr) return Verdict::NoGameObject;
  const void* storage = ReadPointer(game, nevr_patch_addresses::GAME_CALL_QUEUE_OFFSET);
  return storage != nullptr ? Verdict::Ready : Verdict::QueueNotAllocated;
}

inline const char* Name(Verdict verdict) {
  switch (verdict) {
    case Verdict::Ready:
      return "ready";
    case Verdict::NoNetGame:
      return "no_net_game";
    case Verdict::NoGameObject:
      return "no_game_object";
    case Verdict::QueueNotAllocated:
      return "queue_not_allocated";
  }
  return "unknown";
}

}  // namespace nevr_return_to_lobby_queue
