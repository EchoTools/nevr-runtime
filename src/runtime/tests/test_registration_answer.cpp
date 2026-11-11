// #2374: a refused registration, and a close before any answer, leave a line an operator can find. The frames are the
// ones the game service sends (nakama server/evr_pipeline_gameserver.go errFailedRegistration: the NEVR protobuf wrapper
// carrying an Error with CODE_REGISTRATION_FAILED, the legacy BroadcasterRegistrationFailure) built as its evr.Marshal does:
// marker, symbol, length, payload, all little-endian, concatenated in one WebSocket write; then, as a second write
// (session_ws.go SendEvrUnrequire -> SendBytes(unrequireBytes)), the STcpConnectionUnrequireEvent, whose payload is the
// event's one Unused byte (evr/common_stcpconnectionunrequireevent.go), not empty.
#include "runtime/server/registration_answer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ixwebsocket/IXWebSocketMessage.h>

#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/websocket_client.h"

namespace {

constexpr uint64_t kMarker = 0xBB8CE7A278BB40F6ULL;
constexpr uint64_t kUnrequireEvent = 0x43e6963ac76beee4ULL;  // STcpConnectionUnrequireEvent, a 1-byte payload
constexpr char kServiceText[] = "refused-because-secret-detail-xyz";

void AppendU64(std::string& out, uint64_t value) { out.append(reinterpret_cast<const char*>(&value), sizeof(value)); }

std::string EvrMessage(uint64_t symbol, const std::string& payload) {
  std::string out;
  AppendU64(out, kMarker);
  AppendU64(out, symbol);
  AppendU64(out, payload.size());
  return out + payload;
}

std::string WrapperMessage(const gameservice::v1::Envelope& envelope) {
  std::string bytes;
  EXPECT_TRUE(envelope.SerializeToString(&bytes));
  return EvrMessage(nevr_registration_answer::kProtobufWrapper, bytes);
}

std::string ErrorWrapper(int code) {
  gameservice::v1::Envelope envelope;
  envelope.mutable_error()->set_code(static_cast<gameservice::v1::Error_Code>(code));
  envelope.mutable_error()->set_message(kServiceText);
  return WrapperMessage(envelope);
}

std::string SuccessWrapper() {
  gameservice::v1::Envelope envelope;
  envelope.mutable_game_server_registration_success()->set_server_id(7);
  return WrapperMessage(envelope);
}

// errFailedRegistration's first write: wrapper and legacy failure (one code byte) in one payload.
std::string RefusalPayload(int protobufCode, unsigned char legacyCode) {
  return ErrorWrapper(protobufCode) +
         EvrMessage(nevr_registration_answer::kLegacyFailure, std::string(1, static_cast<char>(legacyCode)));
}

// Its second write: SendEvrUnrequire's separate frame, one Unused byte.
std::string UnrequireWrite() { return EvrMessage(kUnrequireEvent, std::string(1, '\0')); }

struct Lines {
  std::mutex mutex;
  std::vector<std::pair<EchoVR::LogLevel, std::string>> lines;
  void Add(EchoVR::LogLevel level, const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    lines.emplace_back(level, line);
  }
  size_t Count() {
    std::lock_guard<std::mutex> lock(mutex);
    return lines.size();
  }
  bool Has(EchoVR::LogLevel level, const std::string& needle) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& [l, text] : lines) {
      if (l == level && text.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  std::string All() {
    std::lock_guard<std::mutex> lock(mutex);
    std::string all;
    for (const auto& [l, text] : lines) all += text + "\n";
    return all;
  }
};

class RegistrationAnswerTest : public ::testing::Test {
 protected:
  RegistrationAnswerTest()
      : tracker([this](EchoVR::LogLevel level, const std::string& line) { log.Add(level, line); }) {
    nevr_registration_answer::Attach(client, tracker);
  }

  void Deliver(ix::WebSocketMessageType type, const std::string& frame = {}, uint16_t closeCode = 1005) {
    ix::WebSocketErrorInfo error;
    auto message = std::make_unique<ix::WebSocketMessage>(
        type, frame, frame.size(), error, ix::WebSocketOpenInfo(),
        ix::WebSocketCloseInfo(closeCode, std::string(), false), true);
    client.TestDeliverMessage(message);
  }
  void Open() { Deliver(ix::WebSocketMessageType::Open); }
  void Frame(const std::string& payload) { Deliver(ix::WebSocketMessageType::Message, payload); }
  void Close(uint16_t code = 1005) { Deliver(ix::WebSocketMessageType::Close, {}, code); }

  Lines log;
  nevr_registration_answer::Tracker tracker;
  WebSocketClient client;
};

}  // namespace

TEST_F(RegistrationAnswerTest, TheServicesRefusalIsOneLineNamingBothCodes) {
  Open();
  Frame(RefusalPayload(4, 10));
  Frame(UnrequireWrite());
  Close();
  ASSERT_EQ(log.Count(), 1u) << log.All();
  EXPECT_TRUE(log.Has(EchoVR::LogLevel::Error,
                      "registration refused by ServerDB protobuf_code=4 protobuf_code_name=CODE_REGISTRATION_FAILED message_bytes=33 "
                      "legacy_code=10 legacy_code_name=Failure legacy_payload_bytes=1 reconnect_count="))
      << log.All();
}

TEST_F(RegistrationAnswerTest, ALegacyRefusalAloneIsLoggedWithTheCodesName) {
  Open();
  Frame(EvrMessage(nevr_registration_answer::kLegacyFailure, std::string(1, static_cast<char>(4))));
  EXPECT_TRUE(log.Has(EchoVR::LogLevel::Error, "protobuf_code=none protobuf_code_name=none message_bytes=none legacy_code=4 legacy_code_name=AccountDoesNotExist legacy_payload_bytes=1"))
      << log.All();
  EXPECT_EQ(log.Count(), 1u);
}

TEST_F(RegistrationAnswerTest, ALegacyRefusalWithNoPayloadSaysSo) {
  Open();
  Frame(EvrMessage(nevr_registration_answer::kLegacyFailure, std::string()));
  EXPECT_TRUE(log.Has(EchoVR::LogLevel::Error, "legacy_code=none legacy_code_name=none legacy_payload_bytes=0")) << log.All();
}

TEST_F(RegistrationAnswerTest, ARegistrationRefusalByProtobufAloneIsLoggedOnceAtTheClose) {
  Open();
  Frame(ErrorWrapper(4));
  EXPECT_EQ(log.Count(), 0u) << "the line waits for the legacy code the service sends with it: " << log.All();
  Close();
  ASSERT_EQ(log.Count(), 1u) << log.All();
  EXPECT_TRUE(log.Has(EchoVR::LogLevel::Error,
                      "registration refused by ServerDB protobuf_code=4 protobuf_code_name=CODE_REGISTRATION_FAILED message_bytes=33 "
                      "legacy_code=none legacy_code_name=none legacy_payload_bytes=none"))
      << log.All();
}

TEST_F(RegistrationAnswerTest, TheProtobufErrorAndTheLegacyFrameInTwoWritesAreStillOneLine) {
  Open();
  Frame(ErrorWrapper(4));
  Frame(EvrMessage(nevr_registration_answer::kLegacyFailure, std::string(1, static_cast<char>(10))));
  Close();
  ASSERT_EQ(log.Count(), 1u) << log.All();
  EXPECT_TRUE(log.Has(EchoVR::LogLevel::Error, "protobuf_code=4 ")) << log.All();
  EXPECT_TRUE(log.Has(EchoVR::LogLevel::Error, "legacy_code=10 ")) << log.All();
}

TEST_F(RegistrationAnswerTest, AProtobufErrorOtherThanARegistrationFailureIsNotCalledARefusal) {
  Open();
  Frame(ErrorWrapper(3));
  ASSERT_EQ(log.Count(), 1u) << log.All();
  EXPECT_TRUE(log.Has(EchoVR::LogLevel::Warning,
                      "ServerDB error before registration was answered code=3 code_name=CODE_BAD_INPUT message_bytes=33 reconnect_count="))
      << log.All();
  EXPECT_EQ(log.All().find("registration refused"), std::string::npos) << log.All();
  Close();
  EXPECT_TRUE(log.Has(EchoVR::LogLevel::Warning, "closed before registration was answered")) << "it did not answer the registration: " << log.All();
}

TEST_F(RegistrationAnswerTest, EveryTrackerLineCarriesTheReconnectCountTheDisconnectLineCarries) {
  // The count is process-wide (WebSocketClient::ReconnectCount, the value the Connected and Disconnected lines carry), so
  // each line is compared with the count read when it was written; the second Open must move it.
  const auto expected = [] { return "reconnect_count=" + std::to_string(WebSocketClient::ReconnectCount()) + " "; };
  Open();
  const std::string first = expected();
  Close();
  Open();
  const std::string second = expected();
  EXPECT_NE(first, second) << "a second open is a reconnect";
  Frame(ErrorWrapper(3));
  Frame(RefusalPayload(4, 10));
  Close();
  ASSERT_EQ(log.Count(), 3u) << log.All();
  const std::string all = log.All();
  const size_t closeLine = all.find("closed before registration was answered");
  ASSERT_NE(closeLine, std::string::npos);
  EXPECT_NE(all.find(first, closeLine), std::string::npos) << all;
  EXPECT_NE(all.find("ServerDB error before registration was answered"), std::string::npos);
  size_t at = 0;
  int withSecond = 0;
  while ((at = all.find(second, at)) != std::string::npos) {
    ++withSecond;
    at += second.size();
  }
  EXPECT_EQ(withSecond, 2) << "the protobuf-error line and the refusal line both carry the count of their connection\n" << all;
}

TEST_F(RegistrationAnswerTest, TheServicesTextNeverReachesTheLog) {
  Open();
  Frame(RefusalPayload(4, 10));
  EXPECT_EQ(log.All().find("secret-detail"), std::string::npos) << log.All();
}

TEST_F(RegistrationAnswerTest, ACloseWithNoAnswerSaysSoWithItsCodeAndAge) {
  Open();
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  Close(1005);
  ASSERT_EQ(log.Count(), 1u) << log.All();
  EXPECT_TRUE(log.Has(EchoVR::LogLevel::Warning, "ServerDB closed before registration was answered code=1005 after_ms=")) << log.All();
  const std::string all = log.All();
  const size_t at = all.find("after_ms=");
  ASSERT_NE(at, std::string::npos);
  EXPECT_GE(std::stoull(all.substr(at + 9)), 30u) << all;
}

TEST_F(RegistrationAnswerTest, ACloseAfterTheRefusalAddsNoSecondComplaint) {
  Open();
  Frame(RefusalPayload(4, 10));
  Frame(UnrequireWrite());
  const size_t before = log.Count();
  Close();
  EXPECT_EQ(log.Count(), before) << log.All();
}

TEST_F(RegistrationAnswerTest, ACloseAfterASuccessIsSilentAndTheNextConnectionStartsUnanswered) {
  Open();
  Frame(SuccessWrapper());
  Close(1000);
  EXPECT_EQ(log.Count(), 0u) << log.All();
  Open();
  Close(1006);
  EXPECT_TRUE(log.Has(EchoVR::LogLevel::Warning, "closed before registration was answered code=1006")) << log.All();
}

TEST_F(RegistrationAnswerTest, ALegacySuccessCountsAsAnAnswer) {
  Open();
  Frame(EvrMessage(nevr_registration_answer::kLegacySuccess, std::string(16, '\0')));
  Close();
  EXPECT_EQ(log.Count(), 0u) << log.All();
}

TEST_F(RegistrationAnswerTest, TheRefusalIsLoggedWithoutTheGameThreadEverDispatching) {
  int dispatched = 0;
  client.SetMessageHandler([&dispatched](EchoVR::SymbolId, VOID*, UINT64) { ++dispatched; });
  Open();
  Frame(RefusalPayload(4, 10));
  Frame(UnrequireWrite());
  Close();
  EXPECT_EQ(dispatched, 0);
  EXPECT_EQ(log.Count(), 1u) << log.All();
}

TEST_F(RegistrationAnswerTest, TheEarlierReceiveObserverStillSeesEveryFrame) {
  WebSocketClient other;
  Lines lines;
  nevr_registration_answer::Tracker local([&lines](EchoVR::LogLevel l, const std::string& t) { lines.Add(l, t); });
  int seen = 0;
  nevr_registration_answer::Attach(other, local, [&seen](EchoVR::SymbolId, VOID*, UINT64) { ++seen; });
  ix::WebSocketErrorInfo error;
  const std::string payload = RefusalPayload(4, 10);
  auto message = std::make_unique<ix::WebSocketMessage>(ix::WebSocketMessageType::Message, payload, payload.size(), error,
                                                        ix::WebSocketOpenInfo(), ix::WebSocketCloseInfo(), true);
  other.TestDeliverMessage(message);
  const std::string unrequire = UnrequireWrite();
  auto second = std::make_unique<ix::WebSocketMessage>(ix::WebSocketMessageType::Message, unrequire, unrequire.size(), error,
                                                       ix::WebSocketOpenInfo(), ix::WebSocketCloseInfo(), true);
  other.TestDeliverMessage(second);
  EXPECT_EQ(seen, 3);
  EXPECT_EQ(lines.Count(), 1u);
}
