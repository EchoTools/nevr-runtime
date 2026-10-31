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

/// The net game of the game context (the runtime's g_pGame), as the game's own GetNetGameFromContext
/// (0x140113a90) reads it; null for a null context. NetGameScheduleReturnToLobby takes this pointer,
/// and the game's own callers pass it (PartyJoinedCB 0x1401896a0 passes its `this`, whose first field
/// it compares with a net game state). Passing the context itself makes the game read its call queue
/// from the wrong object (#730).
inline void* NetGameFromContext(void* context) {
  if (context == nullptr) return nullptr;
  return const_cast<void*>(ReadPointer(context, nevr_patch_addresses::CONTEXT_NETGAME_OFFSET));
}

/// `netGame` is the pointer NetGameScheduleReturnToLobby takes: the net game, NetGameFromContext(g_pGame),
/// never g_pGame itself.
inline Verdict Check(const void* netGame) {
  if (netGame == nullptr) return Verdict::NoNetGame;
  const void* game = ReadPointer(netGame, nevr_patch_addresses::NETGAME_GAME_OFFSET);
  if (game == nullptr) return Verdict::NoGameObject;
  const void* storage = ReadPointer(game, nevr_patch_addresses::GAME_CALL_QUEUE_OFFSET);
  return storage != nullptr ? Verdict::Ready : Verdict::QueueNotAllocated;
}

/// The count at game+0x1F10 (0x1401a4ad0 defers the return to lobby while it is non-zero), or UINT64_MAX when
/// the net game or its game object is not there. For the log line that says why a return did not happen (#1756).
/// `read(from, to, size)` copies `size` bytes and returns false when they cannot be read. A caller off the game
/// thread passes one that cannot fault (ReadProcessMemory on the process itself); the default is a plain copy.
template <typename Reader>
inline uint64_t DeferCount(const void* netGame, Reader read) {
  if (netGame == nullptr) return UINT64_MAX;
  const void* game = nullptr;
  if (!read(static_cast<const unsigned char*>(netGame) + nevr_patch_addresses::NETGAME_GAME_OFFSET, &game,
            sizeof(game)) ||
      game == nullptr) {
    return UINT64_MAX;
  }
  uint64_t count = 0;
  if (!read(static_cast<const unsigned char*>(game) + nevr_patch_addresses::GAME_DEFER_COUNT_OFFSET, &count,
            sizeof(count))) {
    return UINT64_MAX;
  }
  return count;
}

inline uint64_t DeferCount(const void* netGame) {
  return DeferCount(netGame, [](const void* from, void* to, size_t size) {
    std::memcpy(to, from, size);
    return true;
  });
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
