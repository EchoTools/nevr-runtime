#pragma once
// Self-check "friend_request" (#405, docs/engine/remote-log.md): an incoming friend request reaches the runtime
// as one SNSFriendInviteNotify, and the accept, a remove, a withdrawal or a rejection ends it
// (nevr_social_roster::ApplyFriendMessage, the classifier the social facade uses on both platforms). Each change
// of the pending requests is one result:
//   request received  account=<id> pending=<n> received=<R> cleared=<C> duplicate=<D>
//   request duplicate account=<id> pending=<n> ...       a notify for a request that is already pending
//   request cleared   account=<id> reason=<message> pending=<n> ...
// The check keeps its OWN RecentList and feeds it every server message, so it cannot disagree with the facade on
// which symbol adds or ends a request, and it is cleared on LoginSuccess as the facade's is. An end for a request
// it never saw reports nothing. An invite shorter than its two ids, or naming account 0, fails (`malformed`).
//
// What it cannot see: whether the game's recently-met list shows the row, or Add Friend works, on the device (the
// #405 live check needs two game clients). It shows that the notify reached the runtime and the pending requests
// changed. Counts are per session.
//
// Header only on top of the self-check unit (compat/self_check.h), the roster (compat/social_roster.h) and the
// codec's symbols (compat/evr_codec.h); the caller links self_check.cpp.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/self_check.h"
#include "runtime/compat/social_roster.h"

namespace nevr_friend_request_check {

namespace detail {

constexpr std::uint64_t kInviteNotify = 0xca09b0b36bd981b7ULL;  // SNSFriendInviteNotify

struct State {
  std::mutex mutex;
  nevr_social_roster::RecentList pending;  // this check's own view of the pending requests
  std::uint64_t received = 0;
  std::uint64_t cleared = 0;
  std::uint64_t duplicate = 0;
};

inline State& S() {
  static State* const state = new State();  // never destroyed (the Quest sentinel registers no atexit)
  return *state;
}

// Register is idempotent by name, so asking again returns the same check (and survives the unit's test reset).
inline nevr_self_check::CheckId Check() {
  return nevr_self_check::Register(
      {"friend_request", "an incoming friend request is listed as pending, and ends with the accept or a removal",
       nullptr});
}

inline void ResetSession() {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.pending.ClearRequests();
  s.received = s.cleared = s.duplicate = 0;
}

inline std::string Counts(const State& s) {
  return " received=" + std::to_string(s.received) + " cleared=" + std::to_string(s.cleared) +
         " duplicate=" + std::to_string(s.duplicate);
}

}  // namespace detail

/// One message the service sent to the game, with its payload. Acts only on the friend-request messages.
inline void OnServerMessage(std::uint64_t symbol, const std::uint8_t* payload, std::size_t length) {
  detail::State& s = detail::S();
  if (symbol == nevr_evr_codec::kSymLoginSuccess) {  // a new session: the last one's requests are gone
    detail::ResetSession();
    return;
  }
  std::string observed;
  bool pass = true;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    const bool invite = symbol == detail::kInviteNotify;
    if (invite) {
      std::uint64_t account = 0;
      for (int i = 7; payload != nullptr && length >= 16 && i >= 0; --i) account = (account << 8) | payload[8 + i];
      if (payload == nullptr || length < 16 || account == 0) {
        nevr_self_check::RegisterSessionReset(&detail::ResetSession);
        observed = "request invite unreadable length=" + std::to_string(length) + " malformed";
        pass = false;
      }
    }
    if (pass) {
      const nevr_social_roster::RequestEvent event =
          nevr_social_roster::ApplyFriendMessage(s.pending, symbol, payload, length);
      if (event.change == nevr_social_roster::RequestChange::kAdded) {
        ++s.received;
        observed = "request received account=" + std::to_string(event.account) +
                   " pending=" + std::to_string(event.pending) + detail::Counts(s);
      } else if (event.change == nevr_social_roster::RequestChange::kCleared) {
        ++s.cleared;
        observed = "request cleared account=" + std::to_string(event.account) + " reason=" + event.reason +
                   " pending=" + std::to_string(event.pending) + detail::Counts(s);
      } else if (invite && event.account != 0) {
        ++s.duplicate;
        observed = "request duplicate account=" + std::to_string(event.account) +
                   " pending=" + std::to_string(event.pending) + detail::Counts(s);
      } else {
        return;  // not a friend-request message, or an end for a request this check never saw
      }
      nevr_self_check::RegisterSessionReset(&detail::ResetSession);
    }
  }
  nevr_self_check::Report(detail::Check(), observed, pass);
}

/// Test hook: forget the pending requests and the counts.
inline void ResetForTest() { detail::ResetSession(); }

}  // namespace nevr_friend_request_check
