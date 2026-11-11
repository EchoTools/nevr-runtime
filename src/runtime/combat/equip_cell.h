#pragma once

// The selected-tac-mod cell in echovr.exe's .data tail, shared by the ported VeilNet (veilnet.cpp writes the selection)
// and the script guards (script_guard.h read it and count their skips): one definition of where it is.
//  +0x00 u64 the CSymbol64 of the tac mod the local player selected ("[NSUSER] sent profile update: {"ability":...}"), 0
//        until the first selection;
//  +0x08 u64 the gear controller's writer pointer (the controller slice);
//  +0x10 u32 the number of times a guard kept a script's node from running for a tac mod the player did not select;
//  +0x18 u64 the tac mod the controller queried;
//  +0x20 u32 the controller's re-run count.

#include <cstdint>

namespace nevr_combat {

inline constexpr std::uintptr_t kEquipCellRva = 0x2104e80;
inline constexpr std::uintptr_t kEquipSkipsOffset = 0x10;

}  // namespace nevr_combat
