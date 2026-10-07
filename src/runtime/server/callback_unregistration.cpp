#include "runtime/server/callback_unregistration.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace GameServer {

EchoVR::Broadcaster* RecordBroadcasterOwner(ServerContext& context) {
  EchoVR::Lobby* lobby = context.GetLobby();
  EchoVR::Broadcaster* owner = lobby != nullptr ? lobby->broadcaster : nullptr;
  context.GetCallbackRegistry().broadcasterOwner = owner;
  return owner;
}

namespace {

struct NamedHandle {
  const char* name;
  uint16_t handle;
};

using HandleTable = std::array<NamedHandle, kBroadcasterCallbackCount>;

HandleTable BroadcasterHandles(const CallbackRegistry& callbacks) {
  // A plain array so the row count is checked at compile time: a missing row is an error here,
  // not a silently zero-filled tail entry with a null name.
  const NamedHandle rows[] = {
      {"sessionStart", callbacks.sessionStart},
      {"sessionError", callbacks.sessionError},
      {"saveLoadout", callbacks.saveLoadout},
      {"saveLoadoutSuccess", callbacks.saveLoadoutSuccess},
      {"saveLoadoutPartial", callbacks.saveLoadoutPartial},
      {"currentLoadoutRequest", callbacks.currentLoadoutRequest},
      {"currentLoadoutResponse", callbacks.currentLoadoutResponse},
      {"refreshProfileForUser", callbacks.refreshProfileForUser},
      {"refreshProfileFromServer", callbacks.refreshProfileFromServer},
      {"lobbySendClientSettings", callbacks.lobbySendClientSettings},
      {"tierReward", callbacks.tierReward},
      {"topAwards", callbacks.topAwards},
      {"newUnlocks", callbacks.newUnlocks},
      {"reliableStatUpdate", callbacks.reliableStatUpdate},
      {"reliableTeamStatUpdate", callbacks.reliableTeamStatUpdate},
  };
  static_assert(sizeof(rows) / sizeof(rows[0]) == kBroadcasterCallbackCount,
                "one row per UDP broadcaster callback");
  HandleTable table{};
  std::copy(std::begin(rows), std::end(rows), table.begin());
  return table;
}

}  // namespace

size_t CountRegisteredBroadcasterCallbacks(const CallbackRegistry& callbacks) {
  size_t registered = 0;
  for (const NamedHandle& entry : BroadcasterHandles(callbacks)) {
    if (IsLiveBroadcasterHandle(entry.handle)) ++registered;
  }
  return registered;
}

std::string MissingBroadcasterCallbacks(const CallbackRegistry& callbacks) {
  std::string missing;
  for (const NamedHandle& entry : BroadcasterHandles(callbacks)) {
    if (IsLiveBroadcasterHandle(entry.handle)) continue;
    if (!missing.empty()) missing += ", ";
    missing += entry.name;
  }
  return missing;
}

size_t UnregisterBroadcasterCallbacks(EchoVR::Broadcaster* liveOwner,
                                      CallbackRegistry& callbacks,
                                      const BroadcasterUnlisten& unlisten) {
  size_t removed = 0;
  if (liveOwner != nullptr && liveOwner == callbacks.broadcasterOwner && unlisten) {
    for (const NamedHandle& entry : BroadcasterHandles(callbacks)) {
      if (IsLiveBroadcasterHandle(entry.handle)) {
        unlisten(liveOwner, entry.handle);
        ++removed;
      }
    }
  }

  callbacks.Clear();
  return removed;
}

}  // namespace GameServer
