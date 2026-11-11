#pragma once

// The equipment station's icon grids with LOADOUTS (#600, EchoCombat slice 7 PR 4; he_is_the_cat's fix_station_cells.py,
// read as text, reconstructed here as hooks on the station's script, mp_combat_equip_podium_logic e0fdfda9a944e8ff).
//
// The script lays out one grid per gear type: for cells 0 .. count - 1 it sets the icon up and positions it. The count is
// expression d45bb367 (RVA 0x16710): min(total + locks to show, max_icons), max_icons being the int at +0xB0 of the
// script's data. Cells past it are never touched. In the stock game a column never shrinks; with LOADOUTS (the ported
// VeilNet changes a column's item count while the match runs) the cells a longer layout used stay where they were and
// show over the new one. So, for a gear type whose cell count VeilNet publishes:
//  - the count is max_icons, so every cell is positioned;
//  - the position of a cell (expression 34396d3 newposition, RVA 0x1D130: it writes the cell's Vec2 to its first
//    argument) at or past the published count is put far off the canvas.
// The thread data both read is ctx[0x19] (`mov r10, [rdx+0xc8]`): +0x00 the gear type, +0x04 the cell.
//
// The counts are one byte per gear type (0..2) at the RULES mailbox +0x14 (items + the lock; VeilNet writes them,
// veilnet.cpp STATION_CELLS). A type with 0, not published, keeps the stock layout entirely: unlike his patch, which
// ran every grid over max_icons whatever was published.

#include <cstdint>

#include "runtime/combat/rules_state.h"

namespace nevr_combat::station_cells {

inline constexpr std::uint64_t kScript = 0xe0fdfda9a944e8ffULL;
inline constexpr std::uint32_t kCountRva = 0x16710;
inline constexpr std::uint32_t kPositionRva = 0x1D130;
inline constexpr std::uintptr_t kThreadDataOffset = 0xc8;  // ctx[0x19]
inline constexpr std::uintptr_t kMaxIconsOffset = 0xb0;    // in the script's data, [*ctx + 0x68]
inline constexpr float kFar = 100000.0F;

/// The RULES mailbox in echovr.exe's .data tail, for the supported build: the .data page end (0x2105000) less 0x140,
/// as VeilNet computes it at run time (RulesMailboxRva). The station's cell counts are at +0x14.
inline constexpr std::uintptr_t kRulesMailboxRva = rules::kMailboxRva;
inline constexpr std::uintptr_t kCellCountsOffset = rules::kStationCells;

// `mov [rsp+8],rbx; mov [rsp+0x10],rbp; mov [rsp+0x18],rsi; push rdi`
inline constexpr std::uint8_t kCountPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
                                                  0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
// `mov [rsp+8],rbx; mov [rsp+0x18],r8; push rdi; sub rsp,0x70`
inline constexpr std::uint8_t kPositionPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x4c, 0x89, 0x44,
                                                     0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x70};

/// The published cell count for `type`, or 0 when it is not published (or the type is not a station gear type).
std::uint8_t Published(const volatile std::uint8_t* counts, std::int32_t type);

/// The loop count: max_icons for a type whose count is published, else the game's own.
std::int32_t Count(std::int32_t stock, std::int32_t max_icons, std::int32_t type, const volatile std::uint8_t* counts);

/// Whether a cell goes off the canvas: its type's count is published and the cell is at or past it.
bool OffCanvas(std::int32_t type, std::int32_t cell, const volatile std::uint8_t* counts);

/// Registers the two hooks with the script hook table. `counts` is the mailbox's cell counts (game base +
/// kRulesMailboxRva + kCellCountsOffset). Once.
void Register(const volatile std::uint8_t* counts);

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
#endif

}  // namespace nevr_combat::station_cells
