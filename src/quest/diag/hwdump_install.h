#pragma once
// Installs the hardware dump's GOT hooks on libr15's own system queries (#335, part B). Android only.

#include <cstddef>
#include <cstdint>

namespace nevr_quest::hwdump {

struct HookState {
  const char* symbol = "";
  std::uint64_t slot = 0;        // pinned link-time address of the JUMP_SLOT in libr15
  bool installed = false;
  const char* status = "not_run";  // GotStatusName, or "not_run"
  std::uint64_t calls = 0;       // the thunk's own counters (no reporter counter is registered)
  std::uint64_t faults = 0;
};

inline constexpr std::size_t kHookCount = 18;

// Installs every hook; one refused install leaves the others and the game's call intact. Returns how many
// installed. Logs nothing itself: GotHook logs each install with its status.
std::size_t InstallHooks() noexcept;

// The state of every hook, in a fixed order, with the live counters.
void HookStates(HookState (&out)[kHookCount]) noexcept;

}  // namespace nevr_quest::hwdump
