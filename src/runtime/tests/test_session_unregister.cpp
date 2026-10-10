#include "runtime/server/session_unregister.h"

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/protobuf_transport.h"
#include "runtime/server/websocket_client.h"

VOID Log(EchoVR::LogLevel, const CHAR*, ...) {}

namespace {
constexpr uint64_t kWireMagic = 0xBB8CE7A278BB40F6ULL;
constexpr size_t kHeaderSize = sizeof(uint64_t) + sizeof(EchoVR::SymbolId) + sizeof(uint64_t);
constexpr char kLobbySessionId[] = "00112233-4455-6677-8899-aabbccddeeff";

void InitializeRegisteredContext(nevr_game_server::ServerContext& context, bool active) {
  context.Initialize(nullptr, nullptr);
  context.FinalizeInitialization();
  ASSERT_TRUE(context.SetRegistered(true));
  if (active) {
    ASSERT_TRUE(context.StartSession());
  }
  auto state = context.GetSessionState();
  state.active = active;
  state.lobbySessionId = kLobbySessionId;
  context.UpdateSessionState(state);
}

gameservice::v1::Envelope DecodeEnvelopeFrame(const std::string& frame) {
  EXPECT_GE(frame.size(), kHeaderSize);
  uint64_t magic = 0;
  EchoVR::SymbolId symbol = 0;
  uint64_t payloadSize = 0;
  std::memcpy(&magic, frame.data(), sizeof(magic));
  std::memcpy(&symbol, frame.data() + sizeof(magic), sizeof(symbol));
  std::memcpy(&payloadSize, frame.data() + sizeof(magic) + sizeof(symbol), sizeof(payloadSize));
  EXPECT_EQ(magic, kWireMagic);
  EXPECT_EQ(symbol, nevr_game_server::kProtobufMessageSymbol);
  EXPECT_EQ(payloadSize, frame.size() - kHeaderSize);
  gameservice::v1::Envelope envelope;
  EXPECT_TRUE(envelope.ParseFromArray(frame.data() + kHeaderSize, static_cast<int>(payloadSize)));
  return envelope;
}
}  // namespace

TEST(SessionUnregister, SendsSerializedCodeEndedBeforeUnlistenAndDisconnect) {
  nevr_game_server::ServerContext context;
  InitializeRegisteredContext(context, true);
  WebSocketClient client;
  client.TestSetConnected(true);
  std::vector<std::string> order;
  std::string transmittedFrame;
  client.TestSetTransportHandler([&order, &transmittedFrame](const std::string& frame) {
    order.push_back("transport-send");
    transmittedFrame = frame;
    return true;
  });

  const auto result = nevr_game_server::UnregisterRegisteredServer(
      context,
      [&client, &context](const gameservice::v1::Envelope& envelope) {
        EXPECT_TRUE(context.IsSessionActive());
        EXPECT_TRUE(client.IsConnected());
        return nevr_game_server::SendProtobufEnvelope(client, envelope);
      },
      [&client, &order]() {
        order.push_back("clear-queue");
        client.DiscardPendingMessages();
      },
      [&context, &client, &order]() {
        order.push_back("unregister-callbacks");
        EXPECT_TRUE(context.IsRegistered());
        EXPECT_FALSE(context.IsSessionActive());
        EXPECT_TRUE(client.IsConnected());
      },
      [&context, &client, &order]() {
        order.push_back("disconnect");
        EXPECT_TRUE(context.IsRegistered());
        client.TestSetConnected(false);
      });

  EXPECT_TRUE(result.attempted);
  EXPECT_EQ(result.sendResult, nevr_game_server::ProtobufSendResult::AcceptedSent);
  EXPECT_EQ(order, (std::vector<std::string>{"transport-send", "clear-queue", "unregister-callbacks", "disconnect"}));
  ASSERT_FALSE(transmittedFrame.empty());
  const auto envelope = DecodeEnvelopeFrame(transmittedFrame);
  ASSERT_EQ(envelope.message_case(), gameservice::v1::Envelope::kLobbySessionEvent);
  EXPECT_EQ(envelope.lobby_session_event().lobby_session_id(), kLobbySessionId);
  EXPECT_EQ(envelope.lobby_session_event().code(), gameservice::v1::LobbySessionEventMessage::CODE_ENDED);
  EXPECT_EQ(context.GetState(), nevr_game_server::ServerState::Initialized);
  EXPECT_FALSE(context.IsSessionActive());
  EXPECT_TRUE(context.GetSessionState().lobbySessionId.empty());
}

TEST(SessionUnregister, FailedSendStillClearsLocalSessionAndCompletesUnregister) {
  nevr_game_server::ServerContext context;
  InitializeRegisteredContext(context, true);
  WebSocketClient client;
  client.TestSetConnected(true);
  size_t sendAttempts = 0;
  size_t unregisterCalls = 0;
  size_t disconnectCalls = 0;
  client.TestSetTransportHandler([&sendAttempts](const std::string&) {
    ++sendAttempts;
    return false;
  });

  const auto result = nevr_game_server::UnregisterRegisteredServer(
      context,
      [&client](const gameservice::v1::Envelope& envelope) {
        return nevr_game_server::SendProtobufEnvelope(client, envelope);
      },
      [&client]() { client.DiscardPendingMessages(); },
      [&unregisterCalls]() { ++unregisterCalls; },
      [&disconnectCalls]() { ++disconnectCalls; });

  EXPECT_TRUE(result.attempted);
  EXPECT_EQ(result.sendResult, nevr_game_server::ProtobufSendResult::TransportRejected);
  EXPECT_EQ(sendAttempts, 1U);
  EXPECT_EQ(unregisterCalls, 1U);
  EXPECT_EQ(disconnectCalls, 1U);
  EXPECT_EQ(context.GetState(), nevr_game_server::ServerState::Initialized);
  EXPECT_FALSE(context.IsSessionActive());
  EXPECT_TRUE(context.GetSessionState().lobbySessionId.empty());
}

TEST(SessionUnregister, QueuedCodeEndedIsDiscardedBeforeFreshRegistration) {
  nevr_game_server::ServerContext context;
  InitializeRegisteredContext(context, true);
  WebSocketClient client;
  const auto result = nevr_game_server::UnregisterRegisteredServer(
      context,
      [&client](const gameservice::v1::Envelope& envelope) {
        return nevr_game_server::SendProtobufEnvelope(client, envelope);
      },
      [&client]() { client.DiscardPendingMessages(); }, {}, {});

  EXPECT_TRUE(result.attempted);
  EXPECT_EQ(result.sendResult, nevr_game_server::ProtobufSendResult::AcceptedQueued);
  EXPECT_TRUE(client.TestCopyPendingMessages().empty());

  gameservice::v1::Envelope registration;
  registration.mutable_game_server_registration()->set_server_id(42);
  EXPECT_EQ(nevr_game_server::SendProtobufEnvelope(client, registration), nevr_game_server::ProtobufSendResult::AcceptedQueued);
  const auto pending = client.TestCopyPendingMessages();
  ASSERT_EQ(pending.size(), 1U);
  const auto decoded = DecodeEnvelopeFrame(pending.front());
  EXPECT_EQ(decoded.message_case(), gameservice::v1::Envelope::kGameServerRegistration);
}

TEST(SessionUnregister, InactiveAndRepeatedUnregisterDoNotAttemptCodeEndedAgain) {
  nevr_game_server::ServerContext context;
  InitializeRegisteredContext(context, false);
  WebSocketClient client;
  size_t sends = 0;
  const auto sender = [&sends](const gameservice::v1::Envelope&) {
    ++sends;
    return nevr_game_server::ProtobufSendResult::AcceptedSent;
  };
  size_t unregisterCalls = 0;
  const auto unregisterCallbacks = [&unregisterCalls]() { ++unregisterCalls; };
  const auto first = nevr_game_server::UnregisterRegisteredServer(context, sender,
      [&client]() { client.DiscardPendingMessages(); }, unregisterCallbacks, {});
  const auto second = nevr_game_server::UnregisterRegisteredServer(context, sender,
      [&client]() { client.DiscardPendingMessages(); }, unregisterCallbacks, {});

  EXPECT_FALSE(first.attempted);
  EXPECT_FALSE(second.attempted);
  EXPECT_EQ(sends, 0U);
  EXPECT_EQ(unregisterCalls, 2U);
  EXPECT_TRUE(context.GetSessionState().lobbySessionId.empty());
}
