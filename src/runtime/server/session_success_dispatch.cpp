#include "runtime/server/session_success_dispatch.h"

#include "gameservice/v1/gameservice.pb.h"

namespace nevr_game_server {

bool ApplyLobbySessionSuccess(const gameservice::v1::SNSLobbySessionSuccessV5Message& message,
                              std::string& lobbySessionId,
                              const std::function<void()>& commitState,
                              const std::function<void(EncodedMessage&)>& dispatch,
                              std::string* failureReason) {
  EncodedMessage encoded = EncodeLobbySessionSuccessV5(message, failureReason);
  if (encoded.size() == 0) return false;

  if (!message.lobby_id().empty()) lobbySessionId = message.lobby_id();
  if (commitState) commitState();
  if (dispatch) dispatch(encoded);
  return true;
}

}  // namespace nevr_game_server
