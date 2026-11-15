#pragma once

#include <array>
#include <cstdint>

#include "runtime/hook/load_timing_rules.h"

// The PC half of the load timing records (runtime/hook/load_timing_rules.h): the clocks, the one process-wide timer and first-load flag,
// and the wrapper on CncaGameSpace::LoadLevel. One `level_load` record per load, one `boot_timing` record per process.
namespace nevr_load_timing {

/// The first 8 bytes of CncaGameSpace::LoadLevel in echovr.exe (tools/hook_identity_manifest.json). The one definition: the
/// join hold's detour and the timing hook are both written against it.
inline constexpr std::array<std::uint8_t, 8> kLoadLevelPrologue = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48};

/// Hooks CncaGameSpace::LoadLevel (addresses.h CNCAGAMESPACE_LOAD_LEVEL) unless `joinHoldHooksIt`: the join hold's detour
/// is then the one at that address and times the load itself (TimedLoad). Logs the outcome once.
void Install(bool joinHoldHooksIt);

/// Runs `call` (the game's LoadLevel) for the level `levelHash` and logs its `level_load` record. Passes the result on.
std::uint64_t TimedLoad(std::uint64_t levelHash, std::uint64_t (*call)(void*, std::uint64_t, void*), void* gameSpace,
                        std::uint64_t p2, void* p3);

/// A line the game logged (the built-in filter's hook): the engine-ready marker records engine_ready_ms.
void OnGameLine(const char* line);

/// The net game switched to LoggedIn: the first one writes the `boot_timing` record.
void OnLoggedIn();

}  // namespace nevr_load_timing
