#include "runtime/server/session_unregister.h"

#include "gameservice/v1/gameservice.pb.h"

namespace nevr_game_server {

SessionEndResult EndActiveServerSession(ServerContext& context,
                                        const EnvelopeSender& send,
                                        const ServerLifecycleAction& clearPendingMessages) {
  SessionEndResult result;
  if (context.IsSessionActive()) {
    result.attempted = true;
    const SessionState state = context.GetSessionState();
    gameservice::v1::Envelope envelope;
    auto* event = envelope.mutable_lobby_session_event();
    event->set_lobby_session_id(state.lobbySessionId);
    event->set_code(gameservice::v1::LobbySessionEventMessage::CODE_ENDED);
    result.sendResult = send ? send(envelope) : ProtobufSendResult::TransportRejected;
  }

  context.EndSession();
  SessionState state = context.GetSessionState();
  state.active = false;
  state.lobbySessionId.clear();
  context.UpdateSessionState(state);
  if (clearPendingMessages) clearPendingMessages();
  return result;
}

SessionEndResult UnregisterRegisteredServer(ServerContext& context,
                                            const EnvelopeSender& send,
                                            const ServerLifecycleAction& clearPendingMessages,
                                            const ServerLifecycleAction& unregisterCallbacks,
                                            const ServerLifecycleAction& disconnect) {
  const SessionEndResult result = EndActiveServerSession(context, send, clearPendingMessages);
  if (unregisterCallbacks) unregisterCallbacks();
  if (disconnect) disconnect();
  context.SetRegistered(false);
  return result;
}

}  // namespace nevr_game_server
