#include "runtime/server/protobuf_transport.h"

#include <string>

#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/websocket_client.h"

namespace GameServer {

ProtobufSendResult SendProtobufEnvelope(WebSocketClient& client,
                                       const gameservice::v1::Envelope& envelope) {
  std::string binaryData;
  if (!envelope.SerializeToString(&binaryData)) {
    return ProtobufSendResult::SerializationFailed;
  }
  switch (client.SendWithStatus(kProtobufMessageSymbol, binaryData.data(), binaryData.size())) {
    case WebSocketSendStatus::Rejected:
      return ProtobufSendResult::TransportRejected;
    case WebSocketSendStatus::Queued:
      return ProtobufSendResult::AcceptedQueued;
    case WebSocketSendStatus::Sent:
      return ProtobufSendResult::AcceptedSent;
  }
  return ProtobufSendResult::TransportRejected;
}

}  // namespace GameServer
