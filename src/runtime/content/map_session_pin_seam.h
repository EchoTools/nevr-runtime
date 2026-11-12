#pragma once

// The game half of the session pin behind an interface, so the pin's state machine (map_session_pin.cpp) runs in a test
// with no game. map_session_pin_game.cpp is the only production implementation.

#include <cstdint>

namespace nevr_map_session_pin {

constexpr const char* kOwnerPinKey = "nevr_map_pin";
constexpr const char* kOwnerLevelKey = "level";
constexpr const char* kNoticeTitle = "Map version mismatch";
constexpr std::uint64_t kLeaveRetryMs = 10000;

class GameAccess {
 public:
  virtual ~GameAccess() = default;
  // False when the owner-JSON getter or setter is not the code this build expects; nothing is called then.
  virtual bool Callable() = 0;
  virtual bool IsServer() = 0;
  virtual std::uint64_t Lobby() = 0;    // the CNSLobby, 0 when there is none
  virtual std::int32_t NetState() = 0;  // EchoVR::NetGameState
  virtual std::uint64_t OwnerSymbol(std::uint64_t lobby, const char* key) = 0;  // 0 when absent
  virtual void SetOwnerSymbol(std::uint64_t lobby, const char* key, std::uint64_t value) = 0;
  virtual std::uint64_t MainLevel() = 0;  // 0 when the game holds no level
  virtual bool WriteNotice(const char* title, const char* line) = 0;
  virtual bool ScheduleLeave() = 0;  // nevr_return_to_lobby::ScheduleNow
  virtual std::uint64_t NowMs() = 0;
};

void TickWith(GameAccess& game);
std::uint64_t SessionLevelOf(GameAccess& game);

// Forgets the channel view, the mounted pins, the fetch request and the tick state.
void ResetForTest();

}  // namespace nevr_map_session_pin
