#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "runtime/server/server_context.h"

namespace GameServer {

using BroadcasterUnlisten = std::function<void(EchoVR::Broadcaster*, uint16_t)>;

// Records the live lobby's broadcaster as the owner of the UDP handles about to
// be registered: the broadcaster ListenForBroadcasterMessage listens on and the
// one UnregisterAllCallbacks passes back as the live owner. Call before the
// first listen. Without it the owner stays null and UnregisterBroadcasterCallbacks
// clears the registry without ever calling unlisten (issue #117). Returns the
// recorded owner, null when there is no lobby or the lobby has no broadcaster.
// Game-thread-only, like the registry it writes.
EchoVR::Broadcaster* RecordBroadcasterOwner(ServerContext& context);

// Unregisters handles only when the currently live lobby still identifies the
// same owner that accepted them. Zero is the invalid-handle sentinel. TCP
// registry fields are dummy sentinels and are never passed to this function.
size_t UnregisterBroadcasterCallbacks(EchoVR::Broadcaster* liveOwner,
                                      CallbackRegistry& callbacks,
                                      const BroadcasterUnlisten& unlisten);

}  // namespace GameServer
