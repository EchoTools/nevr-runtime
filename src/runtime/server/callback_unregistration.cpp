#include "runtime/server/callback_unregistration.h"

#include <array>

namespace GameServer {

size_t UnregisterBroadcasterCallbacks(EchoVR::Broadcaster* liveOwner,
                                      CallbackRegistry& callbacks,
                                      const BroadcasterUnlisten& unlisten) {
  size_t removed = 0;
  if (liveOwner != nullptr && liveOwner == callbacks.broadcasterOwner && unlisten) {
    const std::array<uint16_t, 15> handles = {
        callbacks.sessionStart,
        callbacks.sessionError,
        callbacks.saveLoadout,
        callbacks.saveLoadoutSuccess,
        callbacks.saveLoadoutPartial,
        callbacks.currentLoadoutRequest,
        callbacks.currentLoadoutResponse,
        callbacks.refreshProfileForUser,
        callbacks.refreshProfileFromServer,
        callbacks.lobbySendClientSettings,
        callbacks.tierReward,
        callbacks.topAwards,
        callbacks.newUnlocks,
        callbacks.reliableStatUpdate,
        callbacks.reliableTeamStatUpdate,
    };
    for (const uint16_t handle : handles) {
      if (handle != 0) {
        unlisten(liveOwner, handle);
        ++removed;
      }
    }
  }

  callbacks.Clear();
  return removed;
}

}  // namespace GameServer
