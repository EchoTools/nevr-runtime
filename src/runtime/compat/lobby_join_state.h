#pragma once
// The lobby this client is in and the lobby it is joining (#1413), as the bridge last saw them. The bridge writes it
// from the lobby frames it decodes (ws_bridge.cpp LogClientLobbyMessage), and the JoinPartyLobby hook reads it
// (patch/lobby_full_backoff.cpp) to skip a party-follow join to a lobby the client already has or is entering.
//
// The frames that set and clear it are the four lobby symbols compat/lobby_frame.h decodes:
//   join request (game to server)   sets the joining lobby;
//   session success (server to game) sets the current lobby and clears the joining one it answers;
//   session failure (server to game) clears the current lobby, and clears the joining one when it answers a join.
// No frame marks leaving a lobby: lobby_frame.h's KindOf knows no leave symbol, so the current lobby is cleared only by
// a failure, never by a leave. The state is therefore a record of the last join the client was answered, not of
// where the game is now.
//
// One mutex guards both GUIDs; the bridge's thread writes and the game's thread reads.

#include <array>
#include <cstdint>
#include <mutex>

namespace nevr_lobby_join_state {

using Guid = std::array<uint8_t, 16>;

class LobbyJoinState {
 public:
  // The game sent a join for `lobby`: it is now joining it.
  void OnJoinRequested(const Guid& lobby) {
    std::lock_guard<std::mutex> lock(mutex_);
    joining_ = lobby;
  }

  // The service accepted the session for `lobby`: it is now in it, and that answers the join.
  void OnSuccess(const Guid& lobby) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = lobby;
    joining_ = Guid{};
  }

  // The service refused the last request. The current lobby is cleared; the joining one only when the refused
  // request was a join (answersJoin), since a failure of a find says nothing about a join in flight.
  void OnFailure(bool answersJoin) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = Guid{};
    if (answersJoin) joining_ = Guid{};
  }

  void Snapshot(Guid* current, Guid* joining) const {
    std::lock_guard<std::mutex> lock(mutex_);
    *current = current_;
    *joining = joining_;
  }

 private:
  mutable std::mutex mutex_;
  Guid current_{};
  Guid joining_{};
};

}  // namespace nevr_lobby_join_state
