#pragma once

// PLAN-v7 H-CTRL (#2242): the gear controller's chain of tac mods, the decision logic of his controller patches.
//
// The controller is the script 4b65d1b009973281 (mp_combat_ability_core in the symbol corpus). Its graph runs one node per
// tac mod in a chain after the stock wraith node. The UMBRAL VEIL branch (the smoke node) follows wraith, and the POWER BOOST and
// POINT branches follow it in that order (fix_smoke.py --with-buff, --with-point). The chain's last node sends its out-port to a tail that runs the chain's
// query node again while what the chain last queried differs from the selection, at most kMaxReruns times per selection,
// so a selection made in the middle of a chain still gets enabled. The equip cell (equip_cell.h) holds the selection
// (VeilNet writes it), the query the chain made (+18), the controller that made it (+08) and the re-run count (+20).
//
// Source of the constants and of the logic: his tools/fix_smoke.py (add_branches, OUT_PORT, the node ids) and
// tools/equip_guard.py (controller: the query store; chain_tail: the tail; the constants). Read as text. No byte of his
// output is copied: what is here is the decision, written as C++, and the bytes his tool writes are not used.
//
// What this file is: pure. ChainTail and Successor take their inputs and return the decision; RunTail reads and writes
// the equip cell through the pointer it is given; RecordQuery writes the cell's two words. No hook is installed and
// nothing is registered with the script hook table, so the module changes nothing in a running game by itself.
//
// NOT DONE (PLAN-v7 section 16, named debt; the three rows of his controller that need code). KNOWN-WRONG #TBD: the controller
// runs stock until the cave pattern is decided and these rows are written.
//   * The new node 0x5613C5A9 (fix_smoke.py: a copy of the barrier's call node with its script hash and flag re-aimed, 851
//     bytes, eleven references re-aimed). "New id enables its copy" needs the engine calls that node makes. The tree names
//     neither the call targets (0x2B938, 0x2B4A8, 0x20CA0, ... in fix_smoke.py FIXUPS) nor the enable function's wrapper
//     beyond the symbol corpus (ability_equip_state_update 3efff939e87f2683, param_is_ability_enabled 43e1ed75065c0148),
//     so the node's body is not written here.
//   * The entry of execute_node, 0xB5C0 (9 bytes, fix_smoke.py EXEC_ENTRY), and the default of next_node, 0x1B2BF (5 bytes,
//     NEXT_DEFAULT): each of his patches is a jump to a code cave. A hook cannot replace the default case's effect without
//     knowing the switch's other cases, and the cave needs executable memory the tree has no pattern for. Not patched here.
//   * The query store, 0x13634 (equip_guard.py CTRL_QUERY, 7 bytes): his patch is a jump to a cave that
//     stores the queried tac mod to the cell. The stored value is in a register in the middle of the function, so no entry
//     hook sees it. RecordQuery is the store's effect; its site is not patched.
// Because no site is patched, no registration entry is exposed: W-REG has nothing from this module to register.

#include <cstddef>
#include <cstdint>

namespace nevr_combat::gear_controller {

// The node ids of the chain (fix_smoke.py WRAITH_NODE, NEW_NODE): the stock wraith node, and the first new node (the smoke
// branch's node, which the new-node cave would run).
inline constexpr std::int64_t kWraithNode = 0x5613C5A8;
inline constexpr std::int64_t kSmokeNode = 0x5613C5A9;

// The chain's first node, the query that sets what the chain queried (equip_guard.py CTRL_QUERY_NODE).
inline constexpr std::int64_t kQueryNode = 0x52A8D314;

// next_node's answer for "the chain ends here" (his `or rax,-1; ret`).
inline constexpr std::int64_t kEnd = -1;

// The out-ports the chain follows: port 0 (the default) and kOutPort (fix_smoke.py OUT_PORT). Any other port ends the chain.
inline constexpr std::uint64_t kDefaultPort = 0;
inline constexpr std::uint64_t kOutPort = 0x33F5911416F09172ULL;

// The chain in the order the branches were added: the wraith node, then each new tac mod's node. Only the smoke branch is
// present here; a further branch appends its node, and the tail then follows the new last node.
inline constexpr std::int64_t kChain[] = {kWraithNode, kSmokeNode};
inline constexpr std::size_t kChainLength = sizeof(kChain) / sizeof(kChain[0]);

// The tac mod the smoke branch enables: CSymbol64("smoke") (equip_guard.py SMOKE; the test pins it against the hash).
inline constexpr std::uint64_t kSmokeSymbol = 0xE32DC7C9D4804FB5ULL;

// At most this many re-runs for one selection (equip_guard.py MAX_RERUNS). The count is checked after it is raised, so the
// chain runs its query at most kMaxReruns more times than the first pass.
inline constexpr std::uint32_t kMaxReruns = 3;

// The equip cell's words the chain uses (equip_cell.h is the one definition; these are its offsets for the chain):
//   +00 u64 the selected tac mod, +08 u64 the writer (the controller that queried), +18 u64 the tac mod queried, +20 u32 re-runs.
inline constexpr std::size_t kSelectedOffset = 0x00;
inline constexpr std::size_t kWriterOffset = 0x08;
inline constexpr std::size_t kQueriedOffset = 0x18;
inline constexpr std::size_t kRerunsOffset = 0x20;

/// The decision at the chain's last node: the next node (kEnd, or kQueryNode to run the query again) and the re-run count
/// to store in the cell.
struct TailStep {
  std::int64_t next;
  std::uint32_t reruns;
};

/// Whether an out-port continues the chain (kDefaultPort or kOutPort).
bool ContinuesOn(std::uint64_t port);

/// The node after chain[index] for out-port `port`: the next chain node when the port continues and there is one, else kEnd.
/// The last node has no successor here: ChainTail decides it.
std::int64_t Successor(std::size_t index, std::uint64_t port);

/// The tail after the chain's last node, given the out-port, the selection, what the chain queried and the re-run count so
/// far. The order of the checks is his: a port that does not continue ends the chain; no selection ends it; a selection the
/// chain already queried ends it and resets the count; otherwise the count goes up (saturating just above kMaxReruns, so
/// it cannot wrap to 0 and start the chain again) and the chain queries again while the count is at most kMaxReruns.
TailStep ChainTail(std::uint64_t port, std::uint64_t selected, std::uint64_t queried, std::uint32_t reruns);

/// RunTail on the equip cell at `cell`: reads the selection, the queried tac mod and the re-run count, decides ChainTail, and
/// writes the count back. Returns the next node. The cell must hold at least kRerunsOffset + 4 bytes.
std::int64_t RunTail(std::uint8_t* cell, std::uint64_t port);

/// The query store's effect (his controller patch at the query node): the tac mod the chain queried goes to +18 and the
/// writer (the controller that made the query) to +08. The cell must hold at least kQueriedOffset + 8 bytes.
void RecordQuery(std::uint8_t* cell, std::uint64_t queried, std::uintptr_t writer);

}  // namespace nevr_combat::gear_controller
