#pragma once
// The party-follow records (#1413): what the JoinPartyLobby hook and the CanFollowParty hook write, decided here so the
// rules are testable without the game. Pure: no clock, no game memory, no log call. The hooks are in
// patch/lobby_full_backoff.cpp.
//
// Two records, both at Info:
//   proceed   the hook passed a party-follow join on to the game's JoinPartyLobby. The game queues that join every frame
//             while it has a lobby to follow, so the record is written once per run of proceeds to the same lobby; the
//             repeats are counted and carried on the next record (repeats=), as the skip line does.
//   decision  the game's CanFollowParty (the answer FollowParty and JoinPartyLobby both take) differs from the last
//             answer written: whether it allows the follow, and the game state and lobby status it read.

#include <cstdint>
#include <string>

#include "runtime/compat/lobby_frame.h"
#include "runtime/compat/lobby_join_state.h"

namespace nevr_follow_record {

using nevr_lobby_join_state::Guid;

// The lobby id prefix an operator reads: the first group of the lobby's GUID text, or "none" for the nil GUID.
inline std::string LobbyPrefix(const Guid& lobby) { return nevr_lobby_frame::FormatGuidOrNone(lobby).substr(0, 8); }

struct ProceedLogState {
  Guid lastLobby{};
  bool inRun = false;
  std::uint64_t repeatsSinceInfo = 0;
};

struct ProceedLogDecision {
  bool writeInfo = false;
  std::uint64_t repeats = 0;  // on a write: the proceeds counted since the previous write
};

// Called once for every join the hook passes on, with the leader's lobby (the nil GUID when the lobby is unknown).
inline ProceedLogDecision NoteProceed(ProceedLogState& state, const Guid& lobby) {
  if (state.inRun && state.lastLobby == lobby) {
    ++state.repeatsSinceInfo;
    return ProceedLogDecision{false, 0};
  }
  ProceedLogDecision decision{true, state.repeatsSinceInfo};
  state.lastLobby = lobby;
  state.inRun = true;
  state.repeatsSinceInfo = 0;
  return decision;
}

// Called when the hook does not pass a join on (it skipped the follow, or held it): the run of proceeds ends, so the
// next proceed writes again even when it names the same lobby.
inline void NoteNoProceed(ProceedLogState& state) { state.inRun = false; }

struct DecisionState {
  bool known = false;
  bool willFollow = false;
  int gameState = 0;
  int lobbyStatus = -1;  // -1: not read (the game reads the lobby status only in states 5 and 9)
};

// True when the answer differs from the one last written (or none was written yet); `state` then holds this answer.
inline bool NoteDecision(DecisionState& state, bool willFollow, int gameState, int lobbyStatus) {
  if (state.known && state.willFollow == willFollow && state.gameState == gameState &&
      state.lobbyStatus == lobbyStatus)
    return false;
  state = DecisionState{true, willFollow, gameState, lobbyStatus};
  return true;
}

}  // namespace nevr_follow_record
