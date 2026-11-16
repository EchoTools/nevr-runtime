#pragma once

// Script guards (#600, EchoCombat slice 7 PR 2): a tac mod's script runs its activation only for the tac mod the player
// selected. The EchoCombat tac mods (UMBRAL VEIL, NEUTRON, OVERCHARGE) are copies of the stock barrier and heal scripts,
// attached to the same player actors as the stock ones, so without a guard a stock script and its copy both answer one
// use. He_is_the_cat's equip_guard.py (barrier_copy, heal_copy; read as text) patches each script's node dispatcher; here
// the dispatcher is wrapped instead (runtime/hook/script_hooks.h): for a guarded node, on a client, once the player has
// selected a tac mod, and when that tac mod is not the script's own, the node is not run (the dispatcher returns 0, its
// "node done"), and the skip is counted in the shared cell (equip_cell.h) the ported VeilNet reports. Otherwise the
// game's dispatcher runs.
//
// Four guards, one per script that carries a use node of a tac mod:
//   * 6a0f689e5b91c666, Repair Matrix, the stock heal (own: "heal"). Its dispatcher is execute_single_node at 0xA200.
//   * 94ba7ca09dcf7b0b, UMBRAL VEIL's copy of the stock barrier b87ba2039b049d5e (own: "smoke"). Its dispatcher is at
//     0x5BE0; its use nodes are the custom-param node and the wall create.
//   * 94ba7ca39fc97e1a, NEUTRON's copy of the same barrier (own: "point"). The same dispatcher and use nodes.
//   * 4b65d1b0088d2682, OVERCHARGE's copy of the stock heal (own: "buff"). The heal dispatcher and its bubble activate.
// The stock barrier b87ba203 has no guard: Cat's tools guard only the copies (fix_smoke.py:377, fix_point.py:134), and
// the stock barrier runs only when the gear controller enables it for the barrier itself.
//
// Each row is keyed by its script's hash, so a copy's rows install when that copy loads and only then. A row is planned
// when its script is held (the copy's file is served, or the stock script is in the install): Plan takes the held set
// and decides which rows are planned; RegisterPlanned installs them. Today combat.cpp (its Register call) calls Register,
// which plans every row as if all four scripts were held; W-REG will pass the held set instead. The rows of a copy are
// not gated by the group that offers the tac mod.
//
// The hot-path rule (docs/adr/0038-hot-path-rule.md): the wrapper sits on the dispatcher of each guarded script
// (execute_single_node at kHealDispatchRva and kBarrierDispatchRva), so it runs once for each node such a script
// executes. Which game events make those calls, and how often, is not established here, and no measurement is in this
// change (ADR 0038 rule 2). What each call costs is known from the code: a call that is not skipped reads the shared
// selection word once and compares the node with the guard's node list (at most two entries); a skipped call adds one
// atomic and one interlocked count and writes one Debug line.
//
// A server runs every node: it has no local player whose selection would choose.

#include <cstddef>
#include <cstdint>

namespace nevr_combat::script_guard {

struct Guard {
  const char* name;
  std::uint64_t script;              // the script's hash
  std::uint32_t dispatch_rva;        // its node dispatcher: std::uint64_t (std::int64_t* thread), node id at thread[2]
  const std::uint8_t* expected;      // the dispatcher's first bytes (the stock script's, which a copy shares)
  std::size_t expected_len;
  std::uint64_t own;                 // the CSymbol64 of the tac mod this script is
  const std::int64_t* nodes;         // the nodes it runs only for its own tac mod
  std::size_t node_count;
};

// The scripts: a copy's name is the hash of its file, which the rekey writes from the stock barrier or heal.
inline constexpr std::uint64_t kHealScript = 0x6a0f689e5b91c666ULL;
inline constexpr std::uint64_t kBarrierSmokeScript = 0x94ba7ca09dcf7b0bULL;
inline constexpr std::uint64_t kBarrierPointScript = 0x94ba7ca39fc97e1aULL;
inline constexpr std::uint64_t kOverchargeScript = 0x4b65d1b0088d2682ULL;

// The tac mods' CSymbol64 values (the hashes of "heal", "smoke", "point" and "buff"; the test pins each).
inline constexpr std::uint64_t kHealSymbol = 0xc8c33e4832761fbcULL;
inline constexpr std::uint64_t kSmokeSymbol = 0xe32dc7c9d4804fb5ULL;
inline constexpr std::uint64_t kPointSymbol = 0xe32dc7cad6864aa4ULL;
inline constexpr std::uint64_t kBuffSymbol = 0xc8c33e48386618b6ULL;

// The dispatchers: execute_single_node of the heal script and of the barrier.
inline constexpr std::uint32_t kHealDispatchRva = 0xA200;
inline constexpr std::uint32_t kBarrierDispatchRva = 0x5BE0;
inline constexpr std::uint8_t kHealDispatchPrologue[] = {0x48, 0x8b, 0x41, 0x10, 0xba, 0x28, 0xac, 0xd6, 0x80};
inline constexpr std::uint8_t kBarrierDispatchPrologue[] = {0x48, 0x8b, 0x41, 0x10, 0xba, 0xfc, 0x16, 0x94, 0xa6};

// The use nodes: the heal's bubble activate (R15NetHealBubbleActivate, node 0xFB9E3F50, FUN_18000ac00), and the barrier's
// custom-param node and wall create.
inline constexpr std::int64_t kHealBubbleActivate = 0xfb9e3f50;
inline constexpr std::int64_t kBarrierCustomParam = 0xd11498f0;
inline constexpr std::int64_t kBarrierWallCreate = 0x6fb98e2c;

/// Whether `guard` keeps node `node` from running: a client, a selection made, not the script's own, a guarded node.
inline bool Skips(const Guard& guard, bool is_server, std::uint64_t selected, std::int64_t node) {
  if (is_server || selected == 0 || selected == guard.own) return false;
  for (std::size_t i = 0; i < guard.node_count; ++i) {
    if (guard.nodes[i] == node) return true;
  }
  return false;
}

/// The guards, in the order Register installs them: the table's order, index 0 the stock heal.
const Guard* Guards(std::size_t* count);

/// The plan: the indexes into Guards() of the guards whose script is in `held` (the script hashes the install serves),
/// written in table order to `out` (room for every guard). Pure: no hook, no log. Returns how many it wrote.
std::size_t Plan(const std::uint64_t* held, std::size_t held_count, std::size_t* out);

/// Registers the guards Plan picks for `held` with the script hook table. Once per process (the first call decides).
void RegisterPlanned(volatile std::uint64_t* equip_cell, bool is_server, const std::uint64_t* held, std::size_t held_count);

/// Registers every guard, as if every script were held. Once.
void Register(volatile std::uint64_t* equip_cell, bool is_server);

/// The skips each guard has counted in this process, for tests and the log.
std::uint64_t SkipsCounted(std::size_t guard);

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
#endif

}  // namespace nevr_combat::script_guard
