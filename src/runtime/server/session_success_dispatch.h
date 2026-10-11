#pragma once

#include <functional>
#include <string>

#include "runtime/server/messages.h"

namespace nevr_game_server {

// Encodes before changing cached session state. A missing callback represents
// a currently unavailable broadcaster; valid state may still be committed.
bool ApplyLobbySessionSuccess(const gameservice::v1::SNSLobbySessionSuccessV5Message& message,
                              std::string& lobbySessionId,
                              const std::function<void()>& commitState,
                              const std::function<void(EncodedMessage&)>& dispatch);

}  // namespace nevr_game_server
