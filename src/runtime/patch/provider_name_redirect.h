#pragma once

#include <cstddef>
#include <cstdint>

// The game's CNSProvider loader (echovr.exe 0x140606690, RVA 0x606690) takes the platform module name as its second
// argument. CreateNetServiceProviders passes "pnsovr" and "pnsdemo" for two of its three provider slots. The hook
// swaps those two names for "pnsrad" before the original runs, so every slot loads pnsrad.dll. Any other name,
// including "pnsrad" itself and a null name, passes through unchanged.
namespace nevr_provider_name_redirect {

/// Installs the detour at gameBase + the loader's RVA, after checking the 24 prologue bytes there. Returns true when
/// the detour is installed. On a prologue mismatch the game is left untouched, a Warning names the expected and actual
/// bytes, and false is returned. A failed detour is reported by PatchDetour. The caller decides what a failure means.
bool Install(std::uintptr_t gameBase);

/// Installs the detour at `target`, after checking `expectedLen` bytes there against `expected`. Install passes the
/// loader's pinned prologue; the test passes a stand-in function's bytes.
bool InstallAt(void* target, const std::uint8_t* expected, std::size_t expectedLen);

}  // namespace nevr_provider_name_redirect
