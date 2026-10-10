#include "quest/net/frame_tap.h"

#include <exception>
#include <string>

#include "runtime/compat/evr_codec.h"

namespace quest_net {

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
      nevr_evr_codec::Message message;
      if (nevr_evr_codec::ReadMessage(copy, offset, &message) != nevr_evr_codec::ReadStatus::Ok) return false;
      if (message.symbol == nevr_evr_codec::kSymLoginSuccess && message.length >= kLoginSuccessMinPayload) {
        *accountId = ReadLe64(message.payload + kLoginSuccessAccountOffset);
        return true;
      }
      offset += nevr_evr_codec::kHeaderSize + static_cast<std::size_t>(message.length);
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

}  // namespace quest_net
