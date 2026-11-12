// CSpinWait::WaitForValue's yield phase (#2110), as the game's own code does it. The rules are header-only so the test can
// drive them without the binary's hook.
//
// The original (echovr.exe 0x141500ed8-0x141500f33) spins with PAUSE up to its spin limit, then calls Sleep with the argument
// `counter <= 0`, where the counter starts at 10 (0x141500efd) and counts down to 0 and stays there. So the first ten yields are
// Sleep(0), and every round after that is Sleep(1). The counter is a count of yields, not a sleep length.
#pragma once

#include <cstdint>

namespace nevr_spin_wait {

// The number of Sleep(0) yields a wait makes before it sleeps 1 ms a round (the original's counter start).
inline constexpr std::uint32_t kYieldRounds = 10;

// The argument of Sleep for the round after `roundsDone` sleeps of this wait have already run: 0 while the yield phase lasts,
// then 1 (the original's `setle(counter <= 0)`).
constexpr std::uint32_t SleepMsForRound(std::uint32_t roundsDone) noexcept {
  return roundsDone < kYieldRounds ? 0u : 1u;
}

}  // namespace nevr_spin_wait
