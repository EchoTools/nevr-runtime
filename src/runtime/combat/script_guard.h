#pragma once

// Script guards (#600, EchoCombat slice 7 PR 2): a tac mod's script runs its activation only for the tac mod the player
// selected. The EchoCombat tac mods (UMBRAL VEIL, OVERCHARGE, NEUTRON) are copies of the stock barrier and heal scripts
// attached to the same player actors as the stock ones, so without a guard a stock script and its copy both answer one
// use. He_is_the_cat's equip_guard.py patches each script's node dispatcher; here the dispatcher is wrapped instead
// (runtime/hook/script_hooks.h): for a guarded node, on a client, once the player has selected a tac mod, and when that
// tac mod is not the script's own, the node is not run (the dispatcher returns 0, its "node done"), and the skip is
// counted in the shared cell (equip_cell.h) the ported VeilNet reports. Otherwise the game's dispatcher runs.
//
// A server runs every node: it has no local player whose selection would choose.
//
// The stock heal script (Repair Matrix, 6a0f689e5b91c666) is the first: its dispatcher execute_single_node (RVA 0xA200,
// `mov rax,[rcx+0x10]` is the node id) runs R15NetHealBubbleActivate as node 0xFB9E3F50 (FUN_18000ac00).

#include <cstddef>
#include <cstdint>

namespace nevr_combat::script_guard {

struct Guard {
  const char* name;
  std::uint64_t script;              // the script's hash
  std::uint32_t dispatch_rva;        // its node dispatcher: std::uint64_t (std::int64_t* thread), node id at thread[2]
  const std::uint8_t* expected;      // the dispatcher's first bytes
  std::size_t expected_len;
  std::uint64_t own;                 // the CSymbol64 of the tac mod this script is
  const std::int64_t* nodes;         // the nodes it runs only for its own tac mod
  std::size_t node_count;
};

inline constexpr std::uint64_t kHealScript = 0x6a0f689e5b91c666ULL;
inline constexpr std::uint64_t kHealSymbol = 0xc8c33e4832761fbcULL;  // CSymbol64("heal")
inline constexpr std::int64_t kHealBubbleActivate = 0xfb9e3f50;
inline constexpr std::uint8_t kHealDispatchPrologue[] = {0x48, 0x8b, 0x41, 0x10, 0xba, 0x28, 0xac, 0xd6, 0x80};

/// Whether `guard` keeps node `node` from running: a client, a selection made, not the script's own, a guarded node.
inline bool Skips(const Guard& guard, bool is_server, std::uint64_t selected, std::int64_t node) {
  if (is_server || selected == 0 || selected == guard.own) return false;
  for (std::size_t i = 0; i < guard.node_count; ++i) {
    if (guard.nodes[i] == node) return true;
  }
  return false;
}

/// The guards, in the order Register installs them.
const Guard* Guards(std::size_t* count);

/// Registers every guard with the script hook table. `equip_cell` is the shared cell (game base + kEquipCellRva). Once.
void Register(volatile std::uint64_t* equip_cell, bool is_server);

/// The skips each guard has counted in this process, for tests and the log.
std::uint64_t SkipsCounted(std::size_t guard);

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
#endif

}  // namespace nevr_combat::script_guard
