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

  // Receive thread, once per frame message. A registration success marks the connection answered; a refusal (the
  // legacy failure, or any protobuf Error before an answer) marks it answered and writes the refusal line.
  void OnFrame(EchoVR::SymbolId msgId, const void* data, size_t size) {
    if (msgId == kLegacyFailure) {
      // Always written: the legacy frame carries the failure code the service chose (BroadcasterRegistrationFailureCode),
      // which the protobuf Error beside it does not.
      answered_.store(true, std::memory_order_release);
      const auto* bytes = static_cast<const unsigned char*>(data);
      std::string line = "[NEVR.SERVERDB] registration refused by ServerDB source=legacy payload_bytes=" + std::to_string(size);
      if (size >= 1 && bytes != nullptr) {
        line += " code=" + std::to_string(bytes[0]) + " code_name=" + nevr_failure_detail::RegistrationFailureCodeName(bytes[0]);
      } else {
        line += " code=none code_name=none";
      }
      line += " — this server was not registered and cannot host a session";
      Emit(EchoVR::LogLevel::Error, line);
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
      answered_.store(true, std::memory_order_release);
      const int code = static_cast<int>(envelope.error().code());
      Emit(EchoVR::LogLevel::Error,
           "[NEVR.SERVERDB] registration refused by ServerDB source=protobuf code=" + std::to_string(code) +
               " code_name=" + ProtobufErrorCodeName(code) + " message_bytes=" + std::to_string(envelope.error().message().size()) +
               " — this server was not registered and cannot host a session");
    }
  }

  // Receive thread, when the socket closes after it was open for afterMs. Each connection starts unanswered.
  void OnClose(int closeCode, uint64_t afterMs) {
    const bool answered = answered_.exchange(false, std::memory_order_acq_rel);
    if (answered) return;
    Emit(EchoVR::LogLevel::Warning,
         "[NEVR.SERVERDB] ServerDB closed before registration was answered code=" + std::to_string(closeCode) +
             " after_ms=" + std::to_string(afterMs) + " (the service sent neither a registration success nor a refusal)");
  }

  bool Answered() const { return answered_.load(std::memory_order_acquire); }

 private:
  void Emit(EchoVR::LogLevel level, const std::string& line) const {
    if (sink_) sink_(level, line);
  }
  LogSink sink_;
  std::atomic<bool> answered_{false};
};

// The wiring the server and its test share. Set before Connect().
// `next` is the receive observer that was set before this one (the ended-session watchdog's); it still sees every frame.
inline void Attach(WebSocketClient& client, Tracker& tracker, WebSocketClient::MessageCallback next = nullptr) {
  client.SetReceiveObserver([&tracker, next = std::move(next)](EchoVR::SymbolId msgId, VOID* data, UINT64 size) {
    tracker.OnFrame(msgId, data, static_cast<size_t>(size));
    if (next) next(msgId, data, size);
  });
  client.SetCloseObserver([&tracker](int code, uint64_t afterMs) { tracker.OnClose(code, afterMs); });
}

}  // namespace nevr_registration_answer
