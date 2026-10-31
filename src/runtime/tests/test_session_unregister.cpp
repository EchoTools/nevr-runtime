#include "runtime/server/session_unregister.h"

#include "runtime/server/ended_session_watchdog.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <future>
#include <memory>
#include <thread>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <ixwebsocket/IXWebSocketMessage.h>

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

// ---------------------------------------------------------------------------------------------------------------
// #1756: a session the service ended has to release its server. The receive observer sees CODE_ENDED on the
// WebSocket thread; the exit watchdog ends the process when the game never leaves. The tests below drive the
// shape of the failing Combat matches: nakama sends CODE_ENDED, the game thread never services the queue (or its
// return to lobby never completes), and nothing else would end the process.
namespace {

std::string CodeEndedFrame(const std::string& sessionId, gameservice::v1::LobbySessionEventMessage::Code code =
                                                             gameservice::v1::LobbySessionEventMessage::CODE_ENDED) {
  WebSocketClient sender;
  sender.TestSetConnected(true);
  std::string frame;
  sender.TestSetTransportHandler([&frame](const std::string& sent) {
    frame = sent;
    return true;
  });
  gameservice::v1::Envelope envelope;
  auto* event = envelope.mutable_lobby_session_event();
  event->set_lobby_session_id(sessionId);
  event->set_code(code);
  EXPECT_EQ(nevr_game_server::SendProtobufEnvelope(sender, envelope), nevr_game_server::ProtobufSendResult::AcceptedSent);
  return frame;
}

void Deliver(WebSocketClient& client, const std::string& frame) {
  ix::WebSocketErrorInfo error;
  auto message = std::make_unique<ix::WebSocketMessage>(ix::WebSocketMessageType::Message, frame, frame.size(), error,
                                                         ix::WebSocketOpenInfo(), ix::WebSocketCloseInfo(), true);
  client.TestDeliverMessage(message);
}

// The wiring gameserver_callbacks.cpp does, with the exit action recorded instead of PerformGracefulShutdown.
void ObserveEndedSession(WebSocketClient& client, nevr_game_server::EndedSessionWatchdog& watchdog,
                         std::chrono::milliseconds delay, std::promise<std::string>& exited) {
  client.SetReceiveObserver([&watchdog, delay, &exited](EchoVR::SymbolId msgId, VOID* data, UINT64 size) {
    std::string ended;
    if (!nevr_game_server::ParseServiceEndedSession(msgId, data, static_cast<size_t>(size), &ended)) return;
    watchdog.Arm(delay, ended, [&exited](const std::string& id) { exited.set_value(id); });
  });
}

}  // namespace

TEST(EndedSessionExit, ServerLeavesWhenCodeEndedArrivesAndTheGameThreadNeverServicesTheQueue) {
  nevr_game_server::EndedSessionWatchdog watchdog;
  WebSocketClient client;
  std::promise<std::string> exited;
  std::atomic<int> gameThreadDispatches{0};
  client.SetMessageHandler([&gameThreadDispatches](EchoVR::SymbolId, VOID*, UINT64) { ++gameThreadDispatches; });
  ObserveEndedSession(client, watchdog, std::chrono::milliseconds(50), exited);

  Deliver(client, CodeEndedFrame(kLobbySessionId));

  auto future = exited.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready) << "the process was never ended";
  EXPECT_EQ(future.get(), kLobbySessionId);
  EXPECT_TRUE(watchdog.Fired());
  EXPECT_EQ(gameThreadDispatches.load(), 0) << "the game thread must not have been needed";
}

TEST(EndedSessionExit, ServerThatLeavesOnItsOwnIsNotEndedTwice) {
  nevr_game_server::EndedSessionWatchdog watchdog;
  WebSocketClient client;
  std::promise<std::string> exited;
  ObserveEndedSession(client, watchdog, std::chrono::milliseconds(300), exited);

  Deliver(client, CodeEndedFrame(kLobbySessionId));
  EXPECT_TRUE(watchdog.Armed());
  // The Arena shape: the game returns to lobby and the state machine's exit runs, or a new session starts.
  watchdog.Cancel();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  EXPECT_FALSE(watchdog.Fired());
  EXPECT_EQ(exited.get_future().wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
}

TEST(EndedSessionExit, SecondCodeEndedDoesNotPushTheDeadlineBack) {
  nevr_game_server::EndedSessionWatchdog watchdog;
  std::atomic<int> expired{0};
  EXPECT_TRUE(watchdog.Arm(std::chrono::milliseconds(50), "first", [&expired](const std::string&) { ++expired; }));
  EXPECT_FALSE(watchdog.Arm(std::chrono::seconds(60), "second", [&expired](const std::string&) { expired += 100; }));
  for (int i = 0; i < 100 && !watchdog.Fired(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  watchdog.Cancel();
  EXPECT_EQ(expired.load(), 1);
}

TEST(EndedSessionExit, OnlyCodeEndedOnTheProtobufSymbolArms) {
  std::string id;
  const std::string ended = CodeEndedFrame(kLobbySessionId);
  const std::string locked = CodeEndedFrame(kLobbySessionId, gameservice::v1::LobbySessionEventMessage::CODE_LOCKED);
  const auto payloadOf = [](const std::string& frame) { return frame.substr(kHeaderSize); };
  const std::string endedPayload = payloadOf(ended);
  const std::string lockedPayload = payloadOf(locked);

  EXPECT_TRUE(nevr_game_server::ParseServiceEndedSession(nevr_game_server::kProtobufMessageSymbol, endedPayload.data(),
                                                         endedPayload.size(), &id));
  EXPECT_EQ(id, kLobbySessionId);
  EXPECT_FALSE(nevr_game_server::ParseServiceEndedSession(nevr_game_server::kProtobufMessageSymbol, lockedPayload.data(),
                                                          lockedPayload.size(), &id));
  EXPECT_FALSE(nevr_game_server::ParseServiceEndedSession(0x1234, endedPayload.data(), endedPayload.size(), &id));
  EXPECT_FALSE(nevr_game_server::ParseServiceEndedSession(nevr_game_server::kProtobufMessageSymbol, nullptr, 0, &id));
  const std::string garbage = "not an envelope \xff\xff\xff";
  EXPECT_FALSE(nevr_game_server::ParseServiceEndedSession(nevr_game_server::kProtobufMessageSymbol, garbage.data(),
                                                          garbage.size(), &id));
}
