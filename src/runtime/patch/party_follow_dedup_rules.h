#pragma once
// The decision the JoinPartyLobby hook takes before a party-follow join (#1413): skip it when the leader's lobby is the
// lobby this client is in or is joining, and that answer is recent. Pure, so the hook and its tests share one decision.
// The state it reads is compat/lobby_join_state.h. The hook is in patch/lobby_full_backoff.cpp.
//
// The window is 30 s. The double join this guards against follows a success or a join request within seconds (the
// measured gaps are in the PR body), so a skip is bounded to that window. After it, a follow of the same lobby goes
// through, so a player who left the lobby can rejoin it.

#include <cstdint>

#include "runtime/compat/lobby_join_state.h"

namespace nevr_party_follow_dedup {

using nevr_lobby_join_state::Guid;
using nevr_lobby_join_state::Stamp;

constexpr std::uint64_t kFollowDedupWindowMs = 30000;

enum class Skip { None, AlreadyIn, AlreadyJoining };

struct Verdict {
  Skip skip = Skip::None;
  std::uint64_t ageMs = 0;  // time since the stamp that matched, when skipped; 0 otherwise
};

inline bool IsNil(const Guid& g) {
  for (const uint8_t b : g) {
    if (b != 0) return false;
  }
  return true;
}

// A stamp matches when its lobby is the leader's and it is younger than the window (age < window). A nil leader is not
// a lobby, so it always goes through to the game's own logic. A nil current or joining lobby never matches. The current
// lobby wins over the joining one, since a lobby the client is in is the stronger fact.
inline Verdict Decide(const Guid& leader, const Stamp& current, const Stamp& joining, std::uint64_t nowMs,
                      std::uint64_t windowMs = kFollowDedupWindowMs) {
  if (IsNil(leader)) return Verdict{};
  const auto ageOf = [nowMs](const Stamp& s) { return nowMs >= s.atMs ? nowMs - s.atMs : 0; };
  if (!IsNil(current.lobby) && leader == current.lobby) {
    const std::uint64_t age = ageOf(current);
    if (age < windowMs) return Verdict{Skip::AlreadyIn, age};
  }
  if (!IsNil(joining.lobby) && leader == joining.lobby) {
    const std::uint64_t age = ageOf(joining);
    if (age < windowMs) return Verdict{Skip::AlreadyJoining, age};
  }
  return Verdict{};
}

}  // namespace nevr_party_follow_dedup
