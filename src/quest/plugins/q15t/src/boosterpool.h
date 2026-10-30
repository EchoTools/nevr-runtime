// Module `boosterpool` (the companion of `levelname`, word row `booster_pool.plain_name`): CR15NetBoosterCS::AllocateExclusive
// keeps the plain pooled-actor name when the lowspec bit is set.
//
// Stock (ELF 0x2110dd4, ReVault 0x2210dd4; the word pin is at ReVault 0x2210f40): when bit 31 of [[this+0x80]+8]+0x7ad8 is set, AllocateExclusive
// computes `CSymbol64::Append(DAT_038af868, "_lowspec", 1, 1)` and asks `CncaGameSpace::AllocatePooledActor(CSymbol64)` for that
// name. With the full-size global_mp only the plain actor exists, so the allocation finds nothing and the booster's
// BodyTypeExclusionFlags reads a null component (docs/quest/q15t/modules/levelname.md). The patch table does it as one code
// word (`tbz` -> `b`, stock 0x36f8012a, replacement 0x14000009); this module does it in a wrapper reached through the GOT word
// of the AllocatePooledActor import, data page only, and no code is written.
//
// Mechanism: the wrapper answers AllocatePooledActor(st, name). It computes the suffixed name the stock would have built,
// Append(plain, "_lowspec", 1, 1), and when `name` equals it, calls the real function with the plain name instead. Every other
// name, from every other caller, reaches the real function unchanged. The result is what the code-word row produces: the
// booster allocation never sees the suffixed name.
//
// Option: Q15T_LEVELNAME (the same as levelname; the two are one feature and must be on together). Default ON; kill file levelname.off (combat_gates.h).
#pragma once

#include <cstdint>

#include "import_hook.h"  // InstallResult

namespace q15t {

// `base`: the game library's load base. status kOk when the GOT word holds the wrapper. On a refusal nothing is left swapped,
// and one ERROR line names the slot, its found and expected words and the swap status.
InstallResult InstallBoosterPool(std::uintptr_t base);

// Allocations whose suffixed name was replaced by the plain one since install.
std::uint64_t BoosterPoolKeptCount();
// Allocations that passed through unchanged because the plain name could not be read.
std::uint64_t BoosterPoolUnreadableCount();

// Test seams.
std::uintptr_t BoosterPoolWrapperForTest();
void BoosterPoolTargetsForTest(std::uintptr_t* real, std::uintptr_t* append, std::uintptr_t* plain_addr);
void SetBoosterPoolTargetsForTest(std::uintptr_t real, std::uintptr_t append, std::uintptr_t plain_addr);
void ResetBoosterPoolForTest();

}  // namespace q15t
