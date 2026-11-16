#pragma once

// The session-start hold of a dedicated server (PLAN-v5 R6-a-2b; docs/adr/0045 decision 10; N152). A legacy session start is forwarded
// to the game only after the session's content is mounted. This file is the pure state machine: no windows.h, no game object, no clock
// of its own (the caller passes the time). The glue that runs the resolve and holds the frame is session_demand.h / .cpp.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cdn/cdn_demand.h"

namespace nevr_session_demand {

// The edge's grace above the core's wait: the core ends a slow fetch at kServerDemandMs, and this covers a final publish after that
// deadline. Chosen, not measured (PLAN-v5 1.4).
inline constexpr std::uint64_t kDemandGraceMs = 10000;
// The longest a session's start is held. At the deadline the session fails; a hold never forwards on a timeout.
inline constexpr std::uint64_t kSessionDemandHoldMs = nevr_cdn::kServerDemandMs + kDemandGraceMs;

enum class Step : std::uint8_t { kNone, kForward, kFail };

constexpr char kAsciiCaseBit = 'a' - 'A';

constexpr char UpperAscii(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - kAsciiCaseBit) : c; }

// Does the held session's own end drop its start? Only that end does (PLAN-v5 R6-a-2b review, F1): an end that names an earlier session,
// or a game-side end of another session, leaves the held start and its accepts standing. The ids are GUID text. FormatGuid writes upper
// case and the service's text is compared without case; "none" (no id) and an empty id match nothing.
inline bool EndsHeldSession(std::string_view heldSessionId, std::string_view endedSessionId) {
  constexpr std::string_view kNoId = "none";
  if (heldSessionId.empty() || endedSessionId.empty() || heldSessionId == kNoId) return false;
  if (heldSessionId.size() != endedSessionId.size()) return false;
  for (std::size_t i = 0; i < heldSessionId.size(); ++i) {
    if (UpperAscii(heldSessionId[i]) != UpperAscii(endedSessionId[i])) return false;
  }
  return true;
}

// One hold of the session start. Game thread only, except OnSessionEnd's shutdown call (session_demand.h, which says what is unprotected).
// The worker never touches it (session_demand.cpp).
class Hold {
 public:
  explicit Hold(std::uint64_t maxWaitMs) : maxWaitMs_(maxWaitMs) {}

  // A new hold; a hold already standing is replaced, and its answer is stale from now on. Returns the ticket of this hold (the first
  // is 1, one more for each Begin).
  std::uint64_t Begin(std::uint64_t nowMs) {
    replaced_ = holding_;
    ++ticket_;
    begunMs_ = nowMs;
    holding_ = true;
    return ticket_;
  }

  bool Holding() const { return holding_; }

  // True when the last Begin replaced a hold that was still standing (N3): that hold's start is never forwarded, and the caller logs it.
  bool Replaced() const { return replaced_; }

  // The ticket of the current hold (the last Begin's), so the log can name the hold it drops.
  std::uint64_t Ticket() const { return ticket_; }

  // An accept that arrives now is queued (true) while a hold stands; false passes it through.
  bool Admit() const { return holding_; }

  // The worker's answer for `ticket`. Only an ok answer for the current hold forwards; a stale or unknown ticket changes nothing.
  Step Finish(std::uint64_t ticket, bool ok) {
    if (!holding_ || ticket != ticket_) return Step::kNone;
    holding_ = false;
    return ok ? Step::kForward : Step::kFail;
  }

  // Called on every game-loop tick. At the deadline the hold fails and ends; it never forwards. A clock that went back counts as no wait.
  Step Poll(std::uint64_t nowMs) {
    if (!holding_) return Step::kNone;
    const std::uint64_t waited = nowMs >= begunMs_ ? nowMs - begunMs_ : 0;
    if (waited < maxWaitMs_) return Step::kNone;
    holding_ = false;
    return Step::kFail;
  }

  // The session ended: the hold is dropped, and a later answer is ignored.
  void Clear() { holding_ = false; }

 private:
  std::uint64_t maxWaitMs_;
  std::uint64_t begunMs_ = 0;
  std::uint64_t ticket_ = 0;
  bool holding_ = false;
  bool replaced_ = false;
};

}  // namespace nevr_session_demand
