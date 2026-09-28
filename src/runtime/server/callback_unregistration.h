#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "runtime/server/server_context.h"

namespace GameServer {

using BroadcasterUnlisten = std::function<void(EchoVR::Broadcaster*, uint16_t)>;

// Unregisters handles only when the currently live lobby still identifies the
// same owner that accepted them. Zero is the invalid-handle sentinel. TCP
// registry fields are dummy sentinels and are never passed to this function.
size_t UnregisterBroadcasterCallbacks(EchoVR::Broadcaster* liveOwner,
                                      CallbackRegistry& callbacks,
                                      const BroadcasterUnlisten& unlisten);

}  // namespace GameServer
