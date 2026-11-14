/* The session map pin's libr15 half on the Quest (ADR 0048 decision 9): the owner-JSON read and the return to lobby, at the addresses
 * sentinel/pinned_targets.h names (session_pin), run as a frame work of the CR15NetGame::Update hook (quest/invite/frame_dispatch.h).
 * The pure part is core/map_session_pin.h. The Quest is never a server (the pin's server half is the PC's), and it has no Log(): its
 * records go to the sentinel log. Each function is used only after the live module's build ID equals LibR15PatchedBuildId() and its
 * first words equal the pinned ones; otherwise the pin is off and logged once.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::maps::pin {

// Proves the two functions and registers the tick, once. False (logged) when this libr15 is not the pinned build or the
// dispatcher is full. Nothing calls it yet: the channel's start (a later piece) is the one caller, so a run that gives no maps
// config installs nothing.
bool InstallTick() noexcept;

// The Quest's main level (rad_hash("<id>_lowspec") for a map) for ConfigAlias's main-level source; 0 until InstallTick proved this
// libr15 or while there is no net game. Game thread only.
std::uint64_t MainLevelNow() noexcept;

}  // namespace nevr_quest::maps::pin
