#include "quest/integration/frame_tap.h"

#include <exception>
#include <string>

#include "runtime/compat/evr_codec.h"

namespace nevr_quest::integration {

namespace {
constexpr std::size_t kLoginSuccessAccountOffset = 24;  // after the 16-byte session id and the 8-byte platform
constexpr std::size_t kLoginSuccessMinPayload = kLoginSuccessAccountOffset + 8;

std::uint64_t ReadLe64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
}  // namespace

bool FindLoginSuccessAccount(std::string_view frame, std::uint64_t* accountId) noexcept {
  try {
    const std::string copy(frame);  // ReadMessage takes a std::string
    std::size_t offset = 0;
    for (;;) {
      EvrCodec::Message message;
      if (EvrCodec::ReadMessage(copy, offset, &message) != EvrCodec::ReadStatus::Ok) return false;
      if (message.symbol == EvrCodec::kSymLoginSuccess && message.length >= kLoginSuccessMinPayload) {
        *accountId = ReadLe64(message.payload + kLoginSuccessAccountOffset);
        return true;
      }
      offset += EvrCodec::kHeaderSize + static_cast<std::size_t>(message.length);
    }
  } catch (const std::exception&) {
    return false;
  }
}

void FrameTap::Handle(bool serverToGame, std::string_view frame) noexcept {
  try {
    if (sinks_.observe) {
      sinks_.observe(serverToGame, reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size());
    }
  } catch (const std::exception&) {
    // A consumer that throws loses this frame only.
  }
  if (!serverToGame || !sinks_.onLoginSuccess) return;
  std::uint64_t account = 0;
  if (!FindLoginSuccessAccount(frame, &account)) return;
  try {
    sinks_.onLoginSuccess(account);
  } catch (const std::exception&) {
    // Same.
  }
}

void FrameTap::ServerToGame(std::string_view frame) noexcept { Handle(true, frame); }
void FrameTap::GameToServer(std::string_view frame) noexcept { Handle(false, frame); }

}  // namespace nevr_quest::integration
