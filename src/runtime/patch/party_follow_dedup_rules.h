#pragma once
// The decision the JoinPartyLobby hook takes before a party-follow join (#1413): skip it when the leader's lobby is the
// lobby this client is in or the one it is joining. Pure, so the hook and its tests share one decision. The state it
// reads is compat/lobby_join_state.h. The hook is in patch/lobby_full_backoff.cpp.

#include <array>
#include <cstdint>

namespace nevr_party_follow_dedup {

using Guid = std::array<uint8_t, 16>;

enum class Skip { None, AlreadyIn, AlreadyJoining };

inline bool IsNil(const Guid& g) {
  for (const uint8_t b : g) {
    if (b != 0) return false;
  }
  return true;
}

// A nil leader lobby is not a lobby, so it always goes through to the game's own logic. A nil current or joining lobby
// never matches. The current lobby wins over the joining one, since a lobby the client is in is the stronger fact.
inline Skip Decide(const Guid& leader, const Guid& current, const Guid& joining) {
  if (IsNil(leader)) return Skip::None;
  if (!IsNil(current) && leader == current) return Skip::AlreadyIn;
  if (!IsNil(joining) && leader == joining) return Skip::AlreadyJoining;
  return Skip::None;
}

}  // namespace nevr_party_follow_dedup
