#include "runtime/server/protobuf_transport.h"

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/websocket_client.h"

VOID Log(EchoVR::LogLevel, const CHAR*, ...) {}

namespace {
constexpr uint64_t kWireMagic = 0xBB8CE7A278BB40F6ULL;
constexpr size_t kHeaderSize = sizeof(uint64_t) + sizeof(EchoVR::SymbolId) + sizeof(uint64_t);

gameservice::v1::Envelope MakeEnvelope() {
  gameservice::v1::Envelope envelope;
  envelope.mutable_error()->set_message("wire-payload");
  return envelope;
}

void ExpectEncodedEnvelope(const std::string& frame, const gameservice::v1::Envelope& expected) {
  ASSERT_GE(frame.size(), kHeaderSize);
  uint64_t magic = 0;
  EchoVR::SymbolId symbol = 0;
  uint64_t payloadSize = 0;
  std::memcpy(&magic, frame.data(), sizeof(magic));
  std::memcpy(&symbol, frame.data() + sizeof(magic), sizeof(symbol));
  std::memcpy(&payloadSize, frame.data() + sizeof(magic) + sizeof(symbol), sizeof(payloadSize));
  EXPECT_EQ(magic, kWireMagic);
  EXPECT_EQ(symbol, GameServer::kProtobufMessageSymbol);
  ASSERT_EQ(payloadSize, frame.size() - kHeaderSize);
  gameservice::v1::Envelope decoded;
  ASSERT_TRUE(decoded.ParseFromArray(frame.data() + kHeaderSize, static_cast<int>(payloadSize)));
  EXPECT_EQ(decoded.SerializeAsString(), expected.SerializeAsString());
}
}  // namespace

TEST(ProtobufTransport, QueuedEnvelopeIsSerializedAsAParseableWireEnvelope) {
  WebSocketClient client;
  const auto envelope = MakeEnvelope();
  EXPECT_EQ(GameServer::SendProtobufEnvelope(client, envelope), GameServer::ProtobufSendResult::AcceptedQueued);
  const auto pending = client.TestCopyPendingMessages();
  ASSERT_EQ(pending.size(), 1U);
  ExpectEncodedEnvelope(pending.front(), envelope);
}

TEST(ProtobufTransport, QueueCapacityRejectsTheNextMessage) {
  WebSocketClient client;
  const auto envelope = MakeEnvelope();
  for (size_t index = 0; index < 256; ++index) {
    EXPECT_EQ(GameServer::SendProtobufEnvelope(client, envelope), GameServer::ProtobufSendResult::AcceptedQueued);
  }
  EXPECT_EQ(GameServer::SendProtobufEnvelope(client, envelope), GameServer::ProtobufSendResult::TransportRejected);
  EXPECT_EQ(client.TestCopyPendingMessages().size(), 256U);
}

TEST(ProtobufTransport, ConnectedTransportFailureIsNotReportedAsAccepted) {
  WebSocketClient client;
  client.TestSetConnected(true);
  size_t attempts = 0;
  client.TestSetTransportHandler([&attempts](const std::string&) {
    ++attempts;
    return false;
  });
  EXPECT_EQ(GameServer::SendProtobufEnvelope(client, MakeEnvelope()), GameServer::ProtobufSendResult::TransportRejected);
  EXPECT_EQ(attempts, 1U);
}

TEST(ProtobufTransport, ConnectedTransportSuccessIsReportedAsSent) {
  WebSocketClient client;
  client.TestSetConnected(true);
  client.TestSetTransportHandler([](const std::string&) { return true; });
  EXPECT_EQ(GameServer::SendProtobufEnvelope(client, MakeEnvelope()), GameServer::ProtobufSendResult::AcceptedSent);
}

// Issue #43. The receive handler forwards payloads to CBroadcaster::ReceiveLocalEvent
// (echovr.exe 0x140F87AA0), whose msg parameter is mutable and is handed on to every
// registered engine listener. A const payload pointer forced const_cast at each
// forwarding site. The payload is a dispatcher-owned copy, so the callback type
// states that it is writable; pin it so a const pointer cannot come back silently.
static_assert(std::is_same_v<WebSocketClient::MessageCallback,
                             std::function<VOID(EchoVR::SymbolId, VOID*, UINT64)>>,
              "MessageCallback must hand the handler a writable, dispatcher-owned payload (issue #43)");

TEST(ReceivedMessageDispatch, PayloadIsAWritableDispatcherOwnedCopy) {
  WebSocketClient client;
  constexpr EchoVR::SymbolId kMsgId = 0x1122334455667788ULL;
  client.TestEnqueueReceivedMessage({kMsgId, {0x01, 0x02, 0x03}});

  size_t calls = 0;
  EchoVR::SymbolId seenId = 0;
  std::vector<UINT8> seenBytes;
  client.SetMessageHandler([&](EchoVR::SymbolId msgId, VOID* data, UINT64 size) {
    ++calls;
    seenId = msgId;
    ASSERT_NE(data, nullptr);
    auto* bytes = static_cast<UINT8*>(data);
    seenBytes.assign(bytes, bytes + size);
    // A downstream engine listener may write through the buffer; that must be
    // a write to memory the dispatcher owns, not to a const object.
    bytes[0] = 0xFF;
  });
  client.ProcessReceivedMessages();

  EXPECT_EQ(calls, 1U);
  EXPECT_EQ(seenId, kMsgId);
  EXPECT_EQ(seenBytes, (std::vector<UINT8>{0x01, 0x02, 0x03}));
}

TEST(ReceivedMessageDispatch, EmptyPayloadIsDeliveredAsNullWithZeroSize) {
  WebSocketClient client;
  constexpr EchoVR::SymbolId kMsgId = 0x42;
  client.TestEnqueueReceivedMessage({kMsgId, {}});

  size_t calls = 0;
  client.SetMessageHandler([&](EchoVR::SymbolId msgId, VOID* data, UINT64 size) {
    ++calls;
    EXPECT_EQ(msgId, kMsgId);
    EXPECT_EQ(data, nullptr);
    EXPECT_EQ(size, 0U);
  });
  client.ProcessReceivedMessages();
  EXPECT_EQ(calls, 1U);
}
