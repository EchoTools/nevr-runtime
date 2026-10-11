#include "runtime/server/websocket_frame.h"

#include <cstring>
#include <utility>

namespace nevr_game_server {
namespace {
constexpr uint64_t kWireMagic = 0xBB8CE7A278BB40F6ULL;
constexpr size_t kHeaderSize = sizeof(uint64_t) + sizeof(EchoVR::SymbolId) + sizeof(uint64_t);
constexpr uint64_t kMaximumPayloadSize = 1024 * 1024;
}

ParsedWebSocketFrame ParseServerDbFrame(std::string_view frame, size_t queueLimit) {
  ParsedWebSocketFrame result;
  if (frame.size() < kHeaderSize) {
    result.status = WebSocketFrameStatus::TooShort;
    result.remainingLength = frame.size();
    return result;
  }

  size_t offset = 0;
  while (offset < frame.size()) {
    const size_t available = frame.size() - offset;
    if (available < kHeaderSize) {
      if (result.status == WebSocketFrameStatus::Complete) {
        result.status = WebSocketFrameStatus::TruncatedHeader;
        result.errorOffset = offset;
        result.remainingLength = available;
      }
      break;
    }

    uint64_t magic = 0;
    std::memcpy(&magic, frame.data() + offset, sizeof(magic));
    if (magic != kWireMagic) {
      if (result.status == WebSocketFrameStatus::Complete) {
        result.status = WebSocketFrameStatus::InvalidMagic;
        result.errorOffset = offset;
      }
      break;
    }

    EchoVR::SymbolId messageId = 0;
    std::memcpy(&messageId, frame.data() + offset + sizeof(magic), sizeof(messageId));
    uint64_t length = 0;
    std::memcpy(&length, frame.data() + offset + sizeof(magic) + sizeof(messageId), sizeof(length));

    const size_t remaining = available - kHeaderSize;
    if (length > remaining) {
      if (result.status == WebSocketFrameStatus::Complete) {
        result.status = WebSocketFrameStatus::TruncatedPayload;
        result.errorOffset = offset;
        result.errorMessageId = messageId;
        result.declaredLength = length;
        result.remainingLength = remaining;
      }
      break;
    }

    const size_t consumed = kHeaderSize + static_cast<size_t>(length);
    if (length > kMaximumPayloadSize) {
      if (result.status == WebSocketFrameStatus::Complete) {
        result.status = WebSocketFrameStatus::OversizedMessage;
        result.errorOffset = offset;
        result.errorMessageId = messageId;
        result.declaredLength = length;
      }
      offset += consumed;
      continue;
    }

    if (result.messages.size() >= queueLimit) {
      result.status = WebSocketFrameStatus::QueueLimit;
      result.errorOffset = offset;
      result.errorMessageId = messageId;
      result.declaredLength = length;
      result.remainingLength = remaining;
      break;
    }

    ReceivedWebSocketMessage received;
    received.msgId = messageId;
    if (length > 0) {
      const size_t payloadOffset = offset + kHeaderSize;
      received.payload.resize(static_cast<size_t>(length));
      std::memcpy(received.payload.data(), frame.data() + payloadOffset, static_cast<size_t>(length));
    }
    result.messages.push_back(std::move(received));
    offset += consumed;
  }

  return result;
}

}  // namespace nevr_game_server
