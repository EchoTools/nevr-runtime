#pragma once
// The early quit lockout's decisions, apart from the hooks that apply them (early_quit_lockout.cpp), so they
// are unit-tested without the game. Field map and evidence: echovr-reconstruction
// docs/earlyquit_field_analysis.md (dc719052).
//
// The game service's penalty timestamp (nakama EarlyQuitPlayerState.PenaltyTimestamp) is the lockout's
// absolute expiry, in Unix seconds; -1 or 0 means no penalty. It reaches the client as `penaltyts`, through
// the login profile and through SNSEarlyQuitUpdateNotification +0x18, and both paths store it with
// CR15NetGame::SetEarlyQuitPenaltyLevel (echovr.exe 0x1401ae2e0) at netGame+0x64820. That setter writes -1 to
// the countdown expiry the scripts read (+0x64828), and ignores a timestamp no newer than the stored one.

#include <cstdint>

namespace EarlyQuitLockout {

// What the countdown should be once the setter has run.
struct Countdown {
  std::int64_t expiry;  // netGame+0x64828: -1 for none
  bool active;          // flags bit 45, which CR15NetGame::Update (0x1401bbdb0) clears at expiry
  bool resetStoredTs;   // write -1 to netGame+0x64820, so the next penalty passes the setter's "newer" check
};

// incomingTs: what the game service sent; storedTs: netGame+0x64820 after the setter ran; now: Unix seconds.
inline Countdown AfterSetPenalty(std::int64_t incomingTs, std::int64_t storedTs, std::int64_t now) {
  // A cleared penalty (-1 or 0) never reaches the client through the setter, which keeps the newer stored
  // timestamp; the game service clearing it must end the lockout.
  if (incomingTs <= 0) return {-1, false, storedTs != -1};
  if (storedTs > now) return {storedTs, true, false};
  return {-1, false, false};  // expired or none: what the setter left
}

// The penalty expression's lockoutcountdownactive output, which the game hard-codes to 0 (0x140d96319): on
// while the countdown has seconds left.
inline bool CountdownActive(std::int64_t countdownSec) { return countdownSec > 0; }

}  // namespace EarlyQuitLockout
