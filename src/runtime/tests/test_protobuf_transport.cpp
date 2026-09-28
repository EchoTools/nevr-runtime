#include "runtime/server/protobuf_transport.h"

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
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
