#pragma once
// The lobby this client is in and the lobby it is joining (#1413), as the bridge last saw them, each with the time the
// bridge saw it. The bridge writes it from the lobby frames it decodes (ws_bridge.cpp LogClientLobbyMessage), and the
// JoinPartyLobby hook reads it (patch/lobby_full_backoff.cpp) to skip a party-follow join to a lobby the client already
// has or is entering.
//
// The frames that set and clear it are the four lobby symbols compat/lobby_frame.h decodes:
//   join request (game to server)    sets the joining lobby, stamped with the time of the request;
//   session success (server to game) sets the current lobby, stamped, and clears the joining one it answers;
//   session failure (server to game) clears the current lobby, and clears the joining one when it answers a join.
// No frame marks leaving a lobby: lobby_frame.h's KindOf knows no leave symbol. So the state is bounded in time instead:
// the hook skips a follow only while the stamp is younger than its window (patch/party_follow_dedup_rules.h). A player
// who left lobby X and follows its leader again after the window is let through.
//
// The clock is monotonic and injected, so a test drives time with no sleep. One mutex guards both stamps; the bridge's
// thread writes and the game's thread reads.

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace nevr_lobby_join_state {

using Guid = std::array<uint8_t, 16>;
using ClockMs = std::uint64_t (*)();

// Milliseconds on the steady (monotonic) clock. Production uses this; it never goes backwards.
inline std::uint64_t SteadyNowMs() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// A lobby and the time the bridge saw it. A nil lobby means none.
struct Stamp {
  Guid lobby{};
  std::uint64_t atMs = 0;
};

class LobbyJoinState {
 public:
  explicit LobbyJoinState(ClockMs clock = &SteadyNowMs) : clock_(clock) {}

  // The game sent a join for `lobby`: it is now joining it.
  void OnJoinRequested(const Guid& lobby) {
    std::lock_guard<std::mutex> lock(mutex_);
    joining_ = Stamp{lobby, clock_()};
  }

  // The service accepted the session for `lobby`: it is now in it, and that answers the join.
  void OnSuccess(const Guid& lobby) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = Stamp{lobby, clock_()};
    joining_ = Stamp{};
  }

  // The service refused the last request. The current lobby is cleared at once; the joining one only when the refused
  // request was a join (answersJoin), since a failure of a find says nothing about a join in flight.
  void OnFailure(bool answersJoin) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = Stamp{};
    if (answersJoin) joining_ = Stamp{};
  }

  void Snapshot(Stamp* current, Stamp* joining) const {
    std::lock_guard<std::mutex> lock(mutex_);
    *current = current_;
    *joining = joining_;
  }

 private:
  mutable std::mutex mutex_;
  ClockMs clock_;
  Stamp current_{};
  Stamp joining_{};
};

}  // namespace nevr_lobby_join_state
