#pragma once

#include <functional>

#include "runtime/server/protobuf_transport.h"
#include "runtime/server/server_context.h"

namespace gameservice::v1 {
class Envelope;
}

namespace GameServer {

using EnvelopeSender = std::function<ProtobufSendResult(const gameservice::v1::Envelope&)>;
using ServerLifecycleAction = std::function<void()>;

struct SessionEndResult {
  bool attempted = false;
  ProtobufSendResult sendResult = ProtobufSendResult::TransportRejected;
};

// Attempts CODE_ENDED while the session/socket are still active, then clears
// local session state and any disconnected outbound queue.
SessionEndResult EndActiveServerSession(ServerContext& context,
                                       const EnvelopeSender& send,
                                       const ServerLifecycleAction& clearPendingMessages);

// Runs the unregister sequence: end session, clear queued messages, unlisten,
// disconnect, then mark the context unregistered.
SessionEndResult UnregisterRegisteredServer(ServerContext& context,
                                            const EnvelopeSender& send,
                                            const ServerLifecycleAction& clearPendingMessages,
                                            const ServerLifecycleAction& unregisterCallbacks,
                                            const ServerLifecycleAction& disconnect);

}  // namespace GameServer
