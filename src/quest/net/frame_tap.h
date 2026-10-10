// Observes the EVR frames the bridge relays, in both directions, for the consumers that follow the
// login connection: the social models (quest_social::ObserveFrames) and the "service accepted the
// login" signal that gives the social facade its local account (quest_social::SetLocalAccount).
//
// Portable and free of the social library: the consumers are injected, so the host test drives it
// with fakes. A frame is one WebSocket message and carries one or more EVR messages
// ([marker 8][symbol 8][length 8][payload]); the walk uses the shared codec.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace quest_net {

struct FrameTapSinks {
  // Every frame, with its direction. May be empty.
  std::function<void(bool serverToGame, const std::uint8_t* data, std::size_t len)> observe;
  // The account id of a server-to-game LoginSuccess ([session 16][platform 8][account 8]). May be empty.
  std::function<void(std::uint64_t accountId)> onLoginSuccess;
};

class FrameTap {
 public:
  explicit FrameTap(FrameTapSinks sinks) : sinks_(std::move(sinks)) {}

  // Never throw: a failing consumer is dropped for this frame and the relay carries on.
  void ServerToGame(std::string_view frame) noexcept;
  void GameToServer(std::string_view frame) noexcept;

 private:
  void Handle(bool serverToGame, std::string_view frame) noexcept;
  FrameTapSinks sinks_;
};

// The LoginSuccess account id in `frame`, if any message in it is one with a full payload.
// (Exposed for the test.)
bool FindLoginSuccessAccount(std::string_view frame, std::uint64_t* accountId) noexcept;

}  // namespace quest_net
