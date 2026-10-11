#pragma once

// The decision half of the empty-server TTL (#58). When the game schedules a return to lobby
// because its session has no players, returning unloads the level and the runtime then exits the
// process (state_machine.cpp). With a TTL configured, the first such request is held for up to
// `ttl` and the process stays up; the hold ends in one of three ways:
//   - a player joins (the live entrant count is non-zero): Cancel, the swallowed request is dropped;
//   - a shutdown is pending (Ctrl+C, a shutdown command): Cancel, the process is exiting anyway;
//   - the TTL elapses: Release, the caller issues the return to lobby it swallowed.
// After a Release the game's queued callback has not run yet and its next tick asks again; those
// requests proceed until a player has been seen, so one empty session spends one TTL, not many.
// A TTL of 0 means no hold at all: every request proceeds, which is the behaviour without this
// feature. Pure: no clock, no game memory, no logging.

#include <cstdint>
#include <string>

namespace nevr_return_to_lobby_hold {

enum class RequestVerdict {
  Proceed,  // issue the return to lobby now
  Hold,       // swallow it, the first request of a new hold; Poll decides later
  HoldAgain,  // swallow it, a repeat of the request already held (the game asks every tick)
};

enum class PollVerdict {
  Keep,     // still holding
  Release,  // TTL elapsed: issue the swallowed return to lobby
  Cancel,   // a player joined or a shutdown is pending: drop it
};

/// The longest TTL honoured: a day. A larger value is almost certainly a unit mistake.
constexpr uint64_t kMaxTtlSeconds = 86400;

/// Parses network.empty_server_ttl_seconds. Absent or empty is 0 (off). A value that is not a
/// plain non-negative integer is 0 with `problem` set; one above kMaxTtlSeconds is clamped with
/// `problem` set.
inline uint64_t ParseTtlSeconds(const char* text, std::string* problem) {
  if (problem != nullptr) problem->clear();
  if (text == nullptr || text[0] == '\0') return 0;
  uint64_t value = 0;
  for (const char* c = text; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9') {
      if (problem != nullptr) *problem = "not a whole number of seconds";
      return 0;
    }
    value = value * 10 + static_cast<uint64_t>(*c - '0');
    if (value > kMaxTtlSeconds) {
      if (problem != nullptr) *problem = "above the one-day limit";
      return kMaxTtlSeconds;
    }
  }
  return value;
}

class Policy {
 public:
  void SetTtlMs(uint64_t ttlMs) { ttlMs_ = ttlMs; }
  uint64_t TtlMs() const { return ttlMs_; }
  bool Holding() const { return holding_; }
  /// TTL 0 and nothing held: Poll has nothing to decide and must cost nothing.
  bool Idle() const { return !holding_ && ttlMs_ == 0; }
  uint64_t HeldSinceMs() const { return heldSinceMs_; }
  /// Requests swallowed by the current (or just-ended) hold, repeats included.
  uint64_t HeldRequests() const { return heldRequests_; }

  RequestVerdict OnReturnRequested(uint64_t nowMs, uint64_t liveEntrants, bool shutdownPending) {
    if (liveEntrants != 0) released_ = false;
    if (ttlMs_ == 0 || shutdownPending) return RequestVerdict::Proceed;
    if (liveEntrants != 0) return RequestVerdict::Proceed;  // a session with players ending is the normal path
    if (released_) return RequestVerdict::Proceed;
    ++heldRequests_;
    if (holding_) return RequestVerdict::HoldAgain;
    holding_ = true;
    heldSinceMs_ = nowMs;
    heldRequests_ = 1;
    return RequestVerdict::Hold;
  }

  PollVerdict Poll(uint64_t nowMs, uint64_t liveEntrants, bool shutdownPending) {
    if (liveEntrants != 0) released_ = false;
    if (!holding_) return PollVerdict::Keep;
    if (shutdownPending || liveEntrants != 0) {
      holding_ = false;
      return PollVerdict::Cancel;
    }
    if (nowMs - heldSinceMs_ >= ttlMs_) {
      holding_ = false;
      released_ = true;
      return PollVerdict::Release;
    }
    return PollVerdict::Keep;
  }

 private:
  uint64_t ttlMs_ = 0;
  bool holding_ = false;
  uint64_t heldSinceMs_ = 0;
  uint64_t heldRequests_ = 0;
  bool released_ = false;
};

/// Polls only when there is something to decide: at TTL 0 (the default) neither callback is
/// called, so the feature being off costs the game thread nothing.
template <typename CountEntrants, typename ShutdownPending>
PollVerdict PollIfActive(Policy& policy, uint64_t nowMs, CountEntrants countEntrants, ShutdownPending shutdownPending) {
  if (policy.Idle()) return PollVerdict::Keep;
  return policy.Poll(nowMs, countEntrants(), shutdownPending());
}

}  // namespace nevr_return_to_lobby_hold
