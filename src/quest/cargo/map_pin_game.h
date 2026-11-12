/* The session pin's libr15 half on the Quest (CDN_DESIGN Rev 3 §6.5, Q2c): what PC9's map_session_pin_game.cpp reads and calls on
 * the PC, at the Quest's addresses, run as a frame work of the CR15NetGame::Update hook (quest/invite/frame_dispatch.h).
 *
 *   the lobby        *(netgame + 0x40) + 0x18, the CNSLobby CR15NetGame::LoadMap reads the owner's "level" from (0x1260c00..10);
 *   the owner JSON   CNSLobby::OwnerSymbol(char const*, CSymbol64) const at 0x192db34 (lobby x0, key x1, fallback x2; x0);
 *   the net state    netgame + 0 (game_layout::kStateOffset); the main level, the game object's (abi/game_level.h);
 *   the notice       the error block (game_layout::kErrorBlockOffset), in SetErrorMessage(4 args)'s layout;
 *   the leave        CR15NetGame::ScheduleReturnToLobby() at 0x124aa6c, the function the PC's ScheduleNow calls.
 *
 * Addresses are libr15 link-time vaddrs, the space pinned_targets.h uses (QuitOnError 0x12713f8 is the same number in both); the
 * disassembly nevr's comments quote sits 0x100000 above them. Each is used only after the live module's build ID equals
 * LibR15PatchedBuildId() and its first instructions equal the words below; otherwise the pin is off and logged once.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::maps::pin {

// Proves the two functions and registers the tick, once. False (logged) when this libr15 is not the pinned build or the
// dispatcher is full. Called by StartFromConfig only, so a run with no maps config installs nothing.
bool InstallTick() noexcept;

// The Quest's main level (rad_hash("<id>_lowspec") for a map) for ConfigAlias's main-level source; 0 until InstallTick proved this
// libr15 or while there is no net game. Game thread only.
std::uint64_t MainLevelNow() noexcept;

}  // namespace nevr_quest::maps::pin
