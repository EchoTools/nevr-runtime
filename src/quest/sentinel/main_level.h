// The game's main level for the Quest plugins (#1106), the twin of the PC's (src/runtime/ext/plugin_loader.cpp, ObserveMainLevel).
//
// The plugin loader reads the level through a source the sentinel sets (plugin_platform.h SetMainLevelSource), and a frame work
// keeps the contexts the plugins already hold current (plugin::RefreshMainLevel). The game object is the frame hook's netgame's
// (netgame+8; the join reads it the same way), and the symbol is read at kQuestMainLevelSymbolOffset (src/abi/game_level.h).
#pragma once

namespace nevr_quest::main_level {

// Sets the source and registers the frame work, once; later calls return the first result. A refused frame work (a full
// dispatcher) is logged: the plugins then keep the level they were given at init. Call before the first plugin plan.
bool Install() noexcept;

}  // namespace nevr_quest::main_level
