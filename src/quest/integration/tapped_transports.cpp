#include "quest/integration/tapped_transports.h"

namespace nevr_quest::integration {

SessionRouter::SendResult TappedGameTransport::Send(SessionRouter::GameId game, std::string_view frame,
                                                    bool binary) {
  const SessionRouter::SendResult result = inner_->Send(game, frame, binary);
  if (result == SessionRouter::SendResult::Sent && tap_ != nullptr) tap_->ServerToGame(frame);
  return result;
}

void TappedGameTransport::Close(SessionRouter::GameId game, std::uint16_t code, std::string_view reason) {
  inner_->Close(game, code, reason);
}

bool TappedRemoteTransport::Open(const SessionRouter::RemoteOpenRequest& request) {
  if (request.role == SessionRouter::Role::Login && !request.standaloneMatchmaker) {
    // A new login session: nothing may be sent on it until the service accepts the login.
    loginAccepted_.store(false, std::memory_order_release);
    loginRemote_.store(request.remote, std::memory_order_release);
  }
  return inner_->Open(request);
}

SessionRouter::SendResult TappedRemoteTransport::Send(SessionRouter::RemoteId remote, std::string_view frame,
                                                      bool binary) {
  const SessionRouter::SendResult result = inner_->Send(remote, frame, binary);
  if (result == SessionRouter::SendResult::Sent && tap_ != nullptr) tap_->GameToServer(frame);
  return result;
}

void TappedRemoteTransport::Close(SessionRouter::RemoteId remote, std::uint16_t code) {
  SessionRouter::RemoteId expected = remote;
  if (loginRemote_.compare_exchange_strong(expected, SessionRouter::kNoRemote, std::memory_order_acq_rel)) {
    loginAccepted_.store(false, std::memory_order_release);
  }
  inner_->Close(remote, code);
}

bool TappedRemoteTransport::SendToLogin(std::string_view frame) {
  if (!loginAccepted_.load(std::memory_order_acquire)) return false;
  const SessionRouter::RemoteId remote = loginRemote_.load(std::memory_order_acquire);
  if (remote == SessionRouter::kNoRemote) return false;
  return inner_->Send(remote, frame, /*binary=*/true) == SessionRouter::SendResult::Sent;
}

}  // namespace nevr_quest::integration
