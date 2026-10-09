// Decorators for the router's two transports that show every relayed frame to a FrameTap and let the
// social facade send its requests on the login session.
//
// quest_net::SessionBridge builds the transports privately, so a consumer cannot see the frames it
// relays; IntegratedBridge composes the same parts with these decorators in between.
//
//   TappedGameTransport    server -> game direction (the router's Send toward the loopback server)
//   TappedRemoteTransport  game -> server direction (the router's Send toward the remote, which also
//                          carries the frames the router itself injects), and the side channel
//
// A frame is shown to the tap only when the inner transport took it (SendResult::Sent), so a
// WouldBlock that the router retries is not observed twice.
#pragma once

#include <atomic>
#include <string_view>

#include "quest/integration/frame_tap.h"
#include "runtime/compat/session_router.h"

namespace nevr_quest::integration {

class TappedGameTransport final : public SessionRouter::GameTransport {
 public:
  TappedGameTransport(SessionRouter::GameTransport* inner, FrameTap* tap) : inner_(inner), tap_(tap) {}
  // Set once, before the router can call Send.
  void SetInner(SessionRouter::GameTransport* inner) { inner_ = inner; }

  SessionRouter::SendResult Send(SessionRouter::GameId game, std::string_view frame, bool binary) override;
  void Close(SessionRouter::GameId game, std::uint16_t code, std::string_view reason) override;
  void SetIdleExempt(SessionRouter::GameId game, bool exempt) override;

 private:
  SessionRouter::GameTransport* inner_;
  FrameTap* tap_;
};

class TappedRemoteTransport final : public SessionRouter::RemoteTransport {
 public:
  TappedRemoteTransport(SessionRouter::RemoteTransport* inner, FrameTap* tap) : inner_(inner), tap_(tap) {}
  void SetInner(SessionRouter::RemoteTransport* inner) { inner_ = inner; }

  bool Open(const SessionRouter::RemoteOpenRequest& request) override;
  SessionRouter::SendResult Send(SessionRouter::RemoteId remote, std::string_view frame, bool binary) override;
  void Close(SessionRouter::RemoteId remote, std::uint16_t code) override;

  // Sends one whole EVR frame on the login session, outside the router's queue. False when there is no
  // login session, or the login has not been accepted yet (a request before LoginSuccess would reach
  // the service unauthenticated), or the transport refused it. Safe from any thread.
  bool SendToLogin(std::string_view frame);

  // The router saw LoginSuccess on the login session: from here SendToLogin may send. The IntegratedBridge
  // calls it from the tap.
  void MarkLoginAccepted() { loginAccepted_.store(true, std::memory_order_release); }

 private:
  SessionRouter::RemoteTransport* inner_;
  FrameTap* tap_;
  std::atomic<SessionRouter::RemoteId> loginRemote_{SessionRouter::kNoRemote};
  std::atomic<bool> loginAccepted_{false};
};

}  // namespace nevr_quest::integration
