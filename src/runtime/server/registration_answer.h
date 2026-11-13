#pragma once

// What ServerDB answered a registration with, and what an operator reads when it did not answer (#2374).
//
// The service refuses a registration by sending the NEVR protobuf wrapper (an Envelope with an Error, code
// CODE_REGISTRATION_FAILED) and the legacy BroadcasterRegistrationFailure (one code byte), then closing the socket.
// The game thread dispatches those frames to the handlers in gameserver_callbacks.cpp only when it reaches
// GameServerLib::Update; a server whose game thread is busy, or whose shutdown starts first, wrote nothing. This
// tracker runs on the WebSocket receive thread through WebSocketClient's observers, so every refusal and every close
// before an answer leaves one line, whatever the game thread does.
//
// Rule 4 (docs/standards/logging.md): the service's error text is never logged, only codes and byte counts.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <string>

#include "abi/echovr.h"
#include "abi/symbols.h"
#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/failure_detail.h"
#include "runtime/server/websocket_client.h"

namespace nevr_registration_answer {

// The wire id of the NEVR protobuf wrapper (NEVRProtobufMessageV1) and of the legacy answers the service sends.
inline constexpr EchoVR::SymbolId kProtobufWrapper = 0x9ee5107d9e29fd63ULL;
inline constexpr EchoVR::SymbolId kLegacyFailure = EchoVR::Symbols::Tcp::LobbyRegistrationFailure;
inline constexpr EchoVR::SymbolId kLegacySuccess = EchoVR::Symbols::Tcp::LobbyRegistrationSuccess;

// gameservice.v1.Error.Code, by number (the schema's names; the generated enum may carry no name table).
inline const char* ProtobufErrorCodeName(int code) {
  switch (code) {
    case 0: return "CODE_RUNTIME_EXCEPTION_UNSPECIFIED";
    case 1: return "CODE_UNRECOGNIZED_PAYLOAD";
    case 2: return "CODE_MISSING_PAYLOAD";
    case 3: return "CODE_BAD_INPUT";
    case 4: return "CODE_REGISTRATION_FAILED";
    case 5: return "CODE_LOBBY_SESSION_NOT_FOUND";
    default: return "UnrecognizedCode";
  }
}

using LogSink = std::function<void(EchoVR::LogLevel level, const std::string& line)>;

class Tracker {
 public:
  explicit Tracker(LogSink sink) : sink_(std::move(sink)) {}

  // Where the `reconnect_count` every line carries comes from (Attach sets it). Set before Connect().
  void SetReconnectCount(std::function<uint32_t()> source) { reconnectCount_ = std::move(source); }

  // Receive thread, once per frame message. A registration success marks the connection answered. A refusal is ONE
  // line carrying both codes the service sends for it. The service writes the protobuf Error and the legacy failure in
  // one WebSocket payload, protobuf first (nakama errFailedRegistration), so the protobuf code is held and the line is
  // written when the legacy frame arrives, which completes it; a connection that closes with only the protobuf Error
  // writes the line at the close, with the legacy fields `none`. A legacy failure alone writes at once.
  void OnFrame(EchoVR::SymbolId msgId, const void* data, size_t size) {
    if (msgId == kLegacyFailure) {
      if (answered_.exchange(true, std::memory_order_acq_rel) && !protobufRefusal_) return;  // answered already, and no line owed
      LegacyFailure legacy;
      legacy.payloadBytes = size;
      const auto* bytes = static_cast<const unsigned char*>(data);
      if (size >= 1 && bytes != nullptr) legacy.code = bytes[0];
      EmitRefusal(legacy);
      return;
    }
    if (answered_.load(std::memory_order_acquire)) return;
    if (msgId == kLegacySuccess) {
      answered_.store(true, std::memory_order_release);
      return;
    }
    if (msgId != kProtobufWrapper || data == nullptr || size == 0) return;
    gameservice::v1::Envelope envelope;
    if (!envelope.ParseFromArray(data, static_cast<int>(size))) return;  // the game-thread handler logs the parse failure
    if (envelope.message_case() == gameservice::v1::Envelope::kGameServerRegistrationSuccess) {
      answered_.store(true, std::memory_order_release);
    } else if (envelope.message_case() == gameservice::v1::Envelope::kError) {
      const int code = static_cast<int>(envelope.error().code());
      const size_t messageBytes = envelope.error().message().size();
      if (code == static_cast<int>(gameservice::v1::Error::CODE_REGISTRATION_FAILED)) {
        answered_.store(true, std::memory_order_release);
        protobufRefusal_ = true;
        protobufCode_ = code;
        protobufMessageBytes_ = messageBytes;
      } else {
        // Not the registration's answer: the connection stays unanswered, so a close still says so.
        Emit(EchoVR::LogLevel::Warning,
             "[NEVR.SERVERDB] ServerDB error before registration was answered code=" + std::to_string(code) +
                 " code_name=" + ProtobufErrorCodeName(code) + " message_bytes=" + std::to_string(messageBytes) +
                 " reconnect_count=" + std::to_string(Reconnects()) + " (this is not the registration's answer)");
      }
    }
  }

  // Receive thread, when the socket closes after it was open for afterMs. Each connection starts unanswered.
  void OnClose(int closeCode, uint64_t afterMs) {
    const bool answered = answered_.exchange(false, std::memory_order_acq_rel);
    if (protobufRefusal_) {
      EmitRefusal(LegacyFailure{-1, 0, false});  // the legacy frame never came: write the line with what was seen
      return;
    }
    if (answered) return;
    Emit(EchoVR::LogLevel::Warning,
         "[NEVR.SERVERDB] ServerDB closed before registration was answered code=" + std::to_string(closeCode) +
             " after_ms=" + std::to_string(afterMs) + " reconnect_count=" + std::to_string(Reconnects()) +
             " (the service sent neither a registration success nor a refusal)");
  }

  bool Answered() const { return answered_.load(std::memory_order_acquire); }

 private:
  struct LegacyFailure {
    int code = -1;  // -1: no code byte seen
    size_t payloadBytes = 0;
    bool seen = true;
  };

  uint32_t Reconnects() const { return reconnectCount_ ? reconnectCount_() : 0; }

  // The one refusal line. `legacy.seen` is false when only the protobuf Error arrived.
  void EmitRefusal(LegacyFailure legacy) {
    std::string line = "[NEVR.SERVERDB] registration refused by ServerDB ";
    if (protobufRefusal_) {
      line += "protobuf_code=" + std::to_string(protobufCode_) + " protobuf_code_name=" + ProtobufErrorCodeName(protobufCode_) +
              " message_bytes=" + std::to_string(protobufMessageBytes_);
    } else {
      line += "protobuf_code=none protobuf_code_name=none message_bytes=none";
    }
    if (!legacy.seen) {
      line += " legacy_code=none legacy_code_name=none legacy_payload_bytes=none";
    } else if (legacy.code >= 0) {
      line += " legacy_code=" + std::to_string(legacy.code) +
              " legacy_code_name=" + nevr_failure_detail::RegistrationFailureCodeName(static_cast<unsigned char>(legacy.code)) +
              " legacy_payload_bytes=" + std::to_string(legacy.payloadBytes);
    } else {
      line += " legacy_code=none legacy_code_name=none legacy_payload_bytes=" + std::to_string(legacy.payloadBytes);
    }
    line += " reconnect_count=" + std::to_string(Reconnects()) + " — this server was not registered and cannot host a session";
    protobufRefusal_ = false;
    Emit(EchoVR::LogLevel::Error, line);
  }

  void Emit(EchoVR::LogLevel level, const std::string& line) const {
    if (sink_) sink_(level, line);
  }
  LogSink sink_;
  std::function<uint32_t()> reconnectCount_;
  std::atomic<bool> answered_{false};
  // Receive thread only: a protobuf registration refusal seen, its line not yet written.
  bool protobufRefusal_ = false;
  int protobufCode_ = 0;
  size_t protobufMessageBytes_ = 0;
};

// The wiring the server and its test share. Set before Connect().
// `next` is the receive observer that was set before this one (the ended-session watchdog's); it still sees every frame.
inline void Attach(WebSocketClient& client, Tracker& tracker, WebSocketClient::MessageCallback next = nullptr) {
  tracker.SetReconnectCount([] { return WebSocketClient::ReconnectCount(); });
  client.SetReceiveObserver([&tracker, next = std::move(next)](EchoVR::SymbolId msgId, VOID* data, UINT64 size) {
    tracker.OnFrame(msgId, data, static_cast<size_t>(size));
    if (next) next(msgId, data, size);
  });
  client.SetCloseObserver([&tracker](int code, uint64_t afterMs) { tracker.OnClose(code, afterMs); });
}

}  // namespace nevr_registration_answer
