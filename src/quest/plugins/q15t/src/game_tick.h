// A game-thread tick: a GOT wrapper on CncaGame::ExclusiveUpdateGame, called once per frame by CncaGame::Render on the
// game thread with x0 = the CR15Game. The wrapper calls the original first, checks the object's vptr, then runs the
// registered callbacks. Data-page hook only (the import's JUMP_SLOT).
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

using TickFn = void (*)(std::uintptr_t game);

// Up to kMaxGameTicks callbacks (16: refresh, weapon_arm, gunconnect, aibots, impactsnd, bvh, dronekill, dronemuzzle and powerboost can
// all be on at once, 9); false when full. Register before InstallGameTick.
inline constexpr int kMaxGameTicks = 16;
bool AddGameTick(TickFn fn);

// How many callbacks are registered.
int GameTickCount();

// `game_vptr_vaddr`: ELF vaddr of CR15Game's vtable pointer value; a call whose object does not carry it runs no callback.
InstallResult InstallGameTick(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr,
                              std::uintptr_t game_vptr_vaddr);

std::uint32_t GameTickCalls();

}  // namespace q15t
