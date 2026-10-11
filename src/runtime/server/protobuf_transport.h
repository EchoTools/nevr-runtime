#pragma once

#include "abi/echovr.h"

class WebSocketClient;
namespace gameservice::v1 {
class Envelope;
}

namespace nevr_game_server {

constexpr EchoVR::SymbolId kProtobufMessageSymbol = 0x9ee5107d9e29fd63ULL;

enum class ProtobufSendResult {
  SerializationFailed,
  TransportRejected,
  AcceptedQueued,
  AcceptedSent,
};

constexpr bool IsProtobufSendAccepted(ProtobufSendResult result) {
  return result == ProtobufSendResult::AcceptedQueued || result == ProtobufSendResult::AcceptedSent;
}

// AcceptedQueued means queued locally. AcceptedSent means accepted by the
// transport. Neither status proves ServerDB received or processed the envelope.
ProtobufSendResult SendProtobufEnvelope(WebSocketClient& client,
                                       const gameservice::v1::Envelope& envelope);

}  // namespace nevr_game_server
