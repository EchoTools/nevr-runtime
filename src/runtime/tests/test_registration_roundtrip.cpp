// #46: server registration against a local fake ServerDB, no live service and no game.
//
// A real WebSocketClient (the runtime's ServerDB client) connects to an ixwebsocket server standing in
// for ServerDB, sends the registration the runtime builds (registration_envelope.h) through the real
// protobuf transport, and receives the registration success through the real frame parser. The fake
// checks what a real ServerDB checks: the bearer header on the upgrade, the frame header, and the
// envelope's fields.
//
// Not covered here (needs the engine or a live Nakama): the engine calling RequestRegistration after
// its login, the HTTP token mint, the matchmaker path (its routing is covered by the N61 tests in
// test_behavioral.cpp), and Nakama's own handling of the registration.

#include <gtest/gtest.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/protobuf_transport.h"
#include "runtime/server/registration_envelope.h"
#include "runtime/server/websocket_client.h"
#include "runtime/server/websocket_frame.h"

namespace {

constexpr uint64_t kWireMagic = 0xBB8CE7A278BB40F6ULL;
constexpr auto kWait = std::chrono::seconds(10);

std::string EncodeFrame(EchoVR::SymbolId symbol, const std::string& payload) {
  std::string frame(sizeof(uint64_t) * 3, '\0');
  const uint64_t length = payload.size();
  std::memcpy(&frame[0], &kWireMagic, sizeof(uint64_t));
  std::memcpy(&frame[sizeof(uint64_t)], &symbol, sizeof(symbol));
  std::memcpy(&frame[sizeof(uint64_t) * 2], &length, sizeof(length));
  return frame + payload;
}

// What ServerDB does with a registration: check it, answer with a registration success.
class FakeServerDb {
 public:
  FakeServerDb() {
    ix::initNetSystem();
    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> ports(49152, 65000);
    for (int attempt = 0; attempt < 20; ++attempt) {
      port_ = ports(gen);
      server_ = std::make_unique<ix::WebSocketServer>(port_, "127.0.0.1");
      server_->disablePerMessageDeflate();
      server_->setOnClientMessageCallback(
          [this](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
            OnMessage(ws, *msg);
          });
      if (server_->listen().first) break;
      server_.reset();
    }
    if (server_) server_->start();
  }
  ~FakeServerDb() {
    if (server_) server_->stop();
  }

  bool Listening() const { return server_ != nullptr; }
  std::string Uri() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/serverdb"; }

  bool WaitForRegistration() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, kWait, [this] { return registered_; });
  }
  std::string Authorization() {
    std::lock_guard<std::mutex> lock(mutex_);
    return authorization_;
  }
  gameservice::v1::GameServerRegistrationMessage Registration() {
    std::lock_guard<std::mutex> lock(mutex_);
    return registration_;
  }
  EchoVR::SymbolId FrameSymbol() {
    std::lock_guard<std::mutex> lock(mutex_);
    return frameSymbol_;
  }

 private:
  void OnMessage(ix::WebSocket& ws, const ix::WebSocketMessage& msg) {
    if (msg.type == ix::WebSocketMessageType::Open) {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = msg.openInfo.headers.find("Authorization");
      authorization_ = it == msg.openInfo.headers.end() ? "<absent>" : it->second;
      return;
    }
    if (msg.type != ix::WebSocketMessageType::Message || !msg.binary) return;
    const nevr_game_server::ParsedWebSocketFrame parsed = nevr_game_server::ParseServerDbFrame(msg.str, 16);
    if (parsed.status != nevr_game_server::WebSocketFrameStatus::Complete || parsed.messages.size() != 1) return;
    gameservice::v1::Envelope envelope;
    if (!envelope.ParseFromArray(parsed.messages[0].payload.data(), static_cast<int>(parsed.messages[0].payload.size())) ||
        envelope.message_case() != gameservice::v1::Envelope::kGameServerRegistration) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      registration_ = envelope.game_server_registration();
      frameSymbol_ = parsed.messages[0].msgId;
      registered_ = true;
    }
    gameservice::v1::Envelope reply;
    reply.mutable_game_server_registration_success()->set_server_id(registration_.server_id());
    reply.mutable_game_server_registration_success()->set_external_ip_address(registration_.internal_ip_address());
    ws.sendBinary(EncodeFrame(nevr_game_server::kProtobufMessageSymbol, reply.SerializeAsString()));
    cv_.notify_all();
  }

  std::unique_ptr<ix::WebSocketServer> server_;
  int port_ = 0;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool registered_ = false;
  std::string authorization_;
  gameservice::v1::GameServerRegistrationMessage registration_;
  EchoVR::SymbolId frameSymbol_ = 0;
};

nevr_game_server::RegistrationParams SampleParams() {
  nevr_game_server::RegistrationParams params;
  params.loginSessionId = "11111111-2222-3333-4444-555555555555";
  params.serverId = 4242;
  params.externalIp = "203.0.113.9";
  params.port = 6792;
  params.regionId = 0xAABBCCDDEEFF0011ULL;
  params.versionLock = 0x1122334455667788ULL;
  params.timeStepUsecs = 16667;
  params.version = nevr_game_server::FormatRegistrationVersion("v4.2.0-5-gabc1234", "abc1234", "Release");
  return params;
}

}  // namespace

TEST(RegistrationEnvelope, CarriesEveryParameterAndAFormattedVersion) {
  const nevr_game_server::RegistrationParams params = SampleParams();
  const gameservice::v1::Envelope envelope = nevr_game_server::BuildRegistrationEnvelope(params);
  ASSERT_EQ(envelope.message_case(), gameservice::v1::Envelope::kGameServerRegistration);
  const auto& r = envelope.game_server_registration();
  EXPECT_EQ(r.login_session_id(), params.loginSessionId);
  EXPECT_EQ(r.server_id(), params.serverId);
  EXPECT_EQ(r.internal_ip_address(), params.externalIp);
  EXPECT_EQ(r.port(), params.port);
  EXPECT_EQ(r.region(), params.regionId);
  EXPECT_EQ(r.version_lock(), params.versionLock);
  EXPECT_EQ(r.time_step_usecs(), params.timeStepUsecs);
  EXPECT_EQ(r.version(), "v4.2.0-5-gabc1234 (abc1234 Release)");
}

TEST(RegistrationRoundTrip, FakeServerDbReceivesTheRegistrationAndTheClientParsesTheSuccess) {
  FakeServerDb serverDb;
  ASSERT_TRUE(serverDb.Listening());

  WebSocketClient client;
  std::mutex mutex;
  std::vector<nevr_game_server::ReceivedWebSocketMessage> received;
  client.SetMessageHandler([&](EchoVR::SymbolId msgId, VOID* data, UINT64 size) {
    nevr_game_server::ReceivedWebSocketMessage message;
    message.msgId = msgId;
    if (data != nullptr) message.payload.assign(static_cast<UINT8*>(data), static_cast<UINT8*>(data) + size);
    std::lock_guard<std::mutex> lock(mutex);
    received.push_back(std::move(message));
  });
  ASSERT_TRUE(client.Connect(serverDb.Uri().c_str(), "fake-token"));

  // Sent before the link is up, the registration is queued and flushed on connect; it is accepted either way.
  const nevr_game_server::RegistrationParams params = SampleParams();
  const nevr_game_server::ProtobufSendResult sent =
      nevr_game_server::SendProtobufEnvelope(client, nevr_game_server::BuildRegistrationEnvelope(params));
  ASSERT_TRUE(nevr_game_server::IsProtobufSendAccepted(sent));
  ASSERT_TRUE(serverDb.WaitForRegistration()) << "ServerDB never received a registration";

  EXPECT_EQ(serverDb.Authorization(), "Bearer fake-token");
  EXPECT_EQ(serverDb.FrameSymbol(), nevr_game_server::kProtobufMessageSymbol);
  const auto registration = serverDb.Registration();
  EXPECT_EQ(registration.login_session_id(), params.loginSessionId);
  EXPECT_EQ(registration.server_id(), params.serverId);
  EXPECT_EQ(registration.internal_ip_address(), params.externalIp);
  EXPECT_EQ(registration.port(), params.port);
  EXPECT_EQ(registration.region(), params.regionId);
  EXPECT_EQ(registration.version_lock(), params.versionLock);
  EXPECT_EQ(registration.version(), params.version);

  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (std::chrono::steady_clock::now() < deadline) {
    client.ProcessReceivedMessages();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!received.empty()) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::lock_guard<std::mutex> lock(mutex);
  ASSERT_EQ(received.size(), 1U) << "the client never delivered the registration success";
  EXPECT_EQ(received[0].msgId, nevr_game_server::kProtobufMessageSymbol);
  gameservice::v1::Envelope reply;
  ASSERT_TRUE(reply.ParseFromArray(received[0].payload.data(), static_cast<int>(received[0].payload.size())));
  ASSERT_EQ(reply.message_case(), gameservice::v1::Envelope::kGameServerRegistrationSuccess);
  EXPECT_EQ(reply.game_server_registration_success().server_id(), params.serverId);
  EXPECT_EQ(reply.game_server_registration_success().external_ip_address(), params.externalIp);
  client.Disconnect();
}
