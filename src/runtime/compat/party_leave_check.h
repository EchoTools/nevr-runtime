#pragma once
// Self-check "party_member_leave" (#403, docs/engine/remote-log.md): the game service tells the remaining members
// of a party that one left with an SNSPartyLeaveNotify {PartyID u64, MemberID u64} (nakama
// evr_pipeline_party.go snsPartyLeaveOnClose, also sent when the member's session closes, so a headset that died
// is told this way). Every notify the game client receives is one result, so a run shows that the notify reached
// it: `leave_notify party=<id> member=<id> notifies=<n>`, passing.
//
// A notify that names no party or no member, or is shorter than its two ids, fails: nakama#681 stopped sending
// a LeaveNotify{PartyID:0} to a party-group peer, and this is how a build would show it coming back.
//
// What it cannot see: why the member left (a leave and a dead headset send the same frame) and the game client's
// own roster. `notifies` counts per session.
//
// Header only on top of the self-check unit (compat/self_check.h) and the party symbol table
// (compat/social_party.h); the caller links self_check.cpp.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/compat/self_check.h"
#include "runtime/compat/social_party.h"

namespace nevr_party_leave_check {

namespace detail {

constexpr std::size_t kPayloadBytes = 16;  // PartyID(8) MemberID(8)

inline std::atomic<std::uint64_t>& Notifies() {
  static std::atomic<std::uint64_t>* const count = new std::atomic<std::uint64_t>(0);  // never destroyed
  return *count;
}

inline std::uint64_t Le64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

// Register is idempotent by name, so asking again returns the same check (and survives the unit's test reset).
inline nevr_self_check::CheckId Check() {
  return nevr_self_check::Register(
      {"party_member_leave", "a member who leaves the party is announced by a well-formed PartyLeaveNotify", nullptr});
}

inline void ResetSession() { Notifies().store(0, std::memory_order_relaxed); }

}  // namespace detail

/// One message symbol the service sent to the game, with its payload. Acts only on SNSPartyLeaveNotify.
inline void OnServerMessage(std::uint64_t symbol, const std::uint8_t* payload, std::size_t length) {
  static const std::uint64_t kLeaveNotify = nevr_social_party::ReplySymbol("PartyLeaveNotify");
  if (symbol == 0 || symbol != kLeaveNotify) return;
  nevr_self_check::RegisterSessionReset(&detail::ResetSession);
  const std::uint64_t n = detail::Notifies().fetch_add(1, std::memory_order_relaxed) + 1;
  std::string observed;
  bool pass = false;
  if (payload == nullptr || length < detail::kPayloadBytes) {
    observed = "leave_notify short length=" + std::to_string(length) + " notifies=" + std::to_string(n) + " malformed";
  } else {
    const std::uint64_t party = detail::Le64(payload);
    const std::uint64_t member = detail::Le64(payload + 8);
    pass = party != 0 && member != 0;
    observed = "leave_notify party=" + std::to_string(party) + " member=" + std::to_string(member) +
               " notifies=" + std::to_string(n) + (pass ? "" : " malformed");
  }
  nevr_self_check::Report(detail::Check(), observed, pass);
}

/// Test hook: forget the count.
inline void ResetForTest() { detail::ResetSession(); }

}  // namespace nevr_party_leave_check
