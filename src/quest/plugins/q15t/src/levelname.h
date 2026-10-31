// Module `levelname` (row R04, the source build's "G7"): CR15Game::LoadLevelName keeps the plain name for global_mp.
//
// Stock (ELF vaddr 0x1202ecc, ReVault 0x1302ecc, 44 bytes): when bit 31 of [this+0x7ad8] is set (Quest "lowspec") the
// level name gets "_lowspec" appended, so global_mp (CSymbol64 0x3F9915D3001DC28E) loads as RAD's cut Quest level. The
// wrapper returns the plain name for global_mp when that bit is set (so the full-size global_mp in the pack loads) and
// leaves every other level to the real function.
//
// Mechanism: the function is only reached through virtual slot +0x260 of the CR15Game vtable address point
// (`ldr x8,[x8,#0x260]; blr x8`, CncaGameSpace::LoadLevel 0x18e5534). The four vtables of the CR15Game class tree
// (CR15Game, CR15NetServer, CR15NetLanServer, CR15NetDedicatedServer) hold the function in that slot as R_AARCH64_ABS64
// relocations in .data.rel.ro; this module swaps those four data words (got_swap.h: data pages only, each word checked
// against base + 0x1202ecc). All four or none: a refusal on any word puts the already swapped ones back.
//
// Companion: `boosterpool` (boosterpool.h, the same option) keeps the plain pooled-actor name in AllocateExclusive. Without it
// the full global_mp crashes the match (docs/quest/q15t/modules/levelname.md). Option macro: Q15T_LEVELNAME, default ON, kill file
// levelname.off (combat_gates.h); a full-size global_mp must be in the pack.
#pragma once

#include <cstdint>

#include "import_hook.h"  // InstallResult

namespace q15t {

// `base`: the game library's load base. status kOk when all four slots hold the wrapper. On a refusal nothing is left
// swapped, `status` is the failing SwapStatus (or kUnexpectedOriginal with reason "already_installed", kNullSlot with
// reason "no_base") and one ERROR/WARNING line says which slot and why.
InstallResult InstallLevelName(std::uintptr_t base);

// global_mp loads that were given the plain name since install.
std::uint64_t LevelNameKeptPlainCount();

// Test seams.
std::uintptr_t LevelNameWrapperForTest();
void LevelNameTargetsForTest(std::uintptr_t* real, std::uintptr_t* plain);       // what a call would reach
void SetLevelNameTargetsForTest(std::uintptr_t real, std::uintptr_t plain);      // fakes for the two callees
void ResetLevelNameForTest();

}  // namespace q15t
