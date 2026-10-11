#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "abi/echovr.h"

namespace nevr_game_server {

struct ReceivedWebSocketMessage {
  EchoVR::SymbolId msgId = 0;
  std::vector<UINT8> payload;
};

enum class WebSocketFrameStatus {
  Complete,
  TooShort,
  InvalidMagic,
  TruncatedHeader,
  TruncatedPayload,
  OversizedMessage,
  QueueLimit,
};

struct ParsedWebSocketFrame {
  std::vector<ReceivedWebSocketMessage> messages;
  WebSocketFrameStatus status = WebSocketFrameStatus::Complete;
  size_t errorOffset = 0;
  EchoVR::SymbolId errorMessageId = 0;
  uint64_t declaredLength = 0;
  size_t remainingLength = 0;
};

// Parses concatenated ServerDB frames in wire order. Identical messages are
// preserved; queueLimit bounds accepted messages while malformed frames stop.
ParsedWebSocketFrame ParseServerDbFrame(std::string_view frame, size_t queueLimit);

}  // namespace nevr_game_server
