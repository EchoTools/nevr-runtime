#include "runtime/server/session_success_dispatch.h"

#include "gameservice/v1/gameservice.pb.h"

namespace GameServer {

bool ApplyLobbySessionSuccess(const gameservice::v1::SNSLobbySessionSuccessV5Message& message,
                              std::string& lobbySessionId,
                              const std::function<void()>& commitState,
                              const std::function<void(EncodedMessage&)>& dispatch) {
  EncodedMessage encoded = EncodeLobbySessionSuccessV5(message);
  if (encoded.size() == 0) return false;

  if (!message.lobby_id().empty()) lobbySessionId = message.lobby_id();
  if (commitState) commitState();
  if (dispatch) dispatch(encoded);
  return true;
}

}  // namespace GameServer
