#pragma once

// The DOT FIELD's remote trigger script, mp_combat_grenade_dot_trigger (4e8af51fe7d6af8c) (#2242, H-DOTT; the port of
// he_is_the_cat's fix_dot_trigger.py, read as text at 82ad7ba). The copy is the stock detonator script (190ed14fe3da5ed1)
// made on the player's machine by the rekey (content_pack_build attaches it; PLAN-v7 4.2). Cat's tool byte-patches the
// copy: three event-id immediates and five next-node rewires. Here the same behaviour is two hooks on the copy, so the
// copy's bytes stay the stock bytes (PLAN-v7 4.3):
//
// 1. Event dispatch (rva 0xB870, FUN_18000B870, per event). The function compares its event (a qword at *rdx) with nine
//    constants, one block each, and reads nothing else of *rdx outside a matched block. Three of the constants are the
//    detonator's onspawnedremote, ondeposited and ontypechanged events (0x0360ce7dfc51dd43, 0x088725a83299f252,
//    0x71b428190a37733e), the ones Cat's tool writes junk over. The hook hands the original a local copy whose event is
//    CSymbol64("nevr_dot_trigger_no_event"), which no block matches, so those three threads never start, as with Cat's
//    immediates. Every other event passes through unchanged.
// 2. Next node (rva 0xBD00, resolve_next_node, per node transition of the script's threads). It takes the node in rcx and
//    the event in rdx. Six table rows (the plan's 8.1 next-node line) return the next node Cat's rewires return: -1 ends the thread
//    (Cat's `or rax,-1; ret`), 0x5a04a5e5 is the client's "disable the trigger component" node. Three rows also match
//    event 0. Any other pair takes the game's next node.
//
// Owns FUN_18000B870 (0xB870..0xBD00) and resolve_next_node (0xBD00..0xCDD0) of 4e8af51fe7d6af8c; inputs: the copy's bytes
// at those two RVAs (the stock detonator's, checked by the script hook table's prologue rows), the event at *rdx of the
// dispatcher, the node in rcx and the event in rdx of resolve_next_node.
//
// Cat's remaining patch sites, the detonator's own onspawnedlocal and autorun paths, are rows of the table above. The
// node-to-site mapping is the plan's table; it is checked here only as far as the compare constants of
// resolve_next_node (both node and event values occur in that function of 190ed14fe3da5ed1).
//
// ADR 0038: both hooks are per-event, not per-frame. The dispatch runs once per event the script receives; resolve_next_node
// once per node transition of a running DOT trigger, a handful per throw. Each does a constant-size compare (three, six),
// so neither is on a per-frame path and no measurement is needed.
//
// The gate is the script hook table's, not a combat group: the DOT FIELD has no combat.* key of its own, and W-REG decides
// where Register is called from combat.cpp.

#include <cstddef>
#include <cstdint>

#include "nevr/symbol_hash.h"

namespace nevr_combat::dot_trigger {

inline constexpr std::uint64_t kScript = 0x4e8af51fe7d6af8cULL;  // mp_combat_grenade_dot_trigger
inline constexpr std::uint32_t kDispatchRva = 0xB870;            // FUN_18000B870 (..0xBD00)
inline constexpr std::uint32_t kNextNodeRva = 0xBD00;            // resolve_next_node (..0xCDD0)

// The bytes at each RVA in the stock detonator script (190ed14fe3da5ed1.dll, revault): the dispatcher's first 16,
// `push rbx; push rdi; sub rsp,0x28; movabs rax, imm64`, and the next-node function's first 10, `mov eax, 0x80c21f22;
// cmp rcx, rax; jg rel32`. The copy's bytes at these two places are the stock bytes, because Cat's patches are elsewhere.
inline constexpr std::uint8_t kDispatchPrologue[] = {0x40, 0x53, 0x57, 0x48, 0x83, 0xec, 0x28, 0x48,
                                                     0xb8, 0xf2, 0xc4, 0xe6, 0xb7, 0x89, 0x48, 0x0c};
inline constexpr std::uint8_t kNextNodePrologue[] = {0xb8, 0x22, 0x1f, 0xc2, 0x80, 0x48, 0x3b, 0xc8, 0x0f, 0x8f};

/// The event id the original is given for a trigger event: a CSymbol64 no block of the dispatcher matches.
inline constexpr std::uint64_t kNoEvent = EchoVR::CSymbol64Hash("nevr_dot_trigger_no_event");

/// The next node a thread's `or rax,-1; ret`: the thread ends.
inline constexpr std::uint64_t kStop = ~std::uint64_t{0};

/// The client's node after the trigger component is disabled for the owner (Cat's 0x5A04A5E5).
inline constexpr std::uint64_t kReleaseTriggerNode = 0x5a04a5e5ULL;

/// One row of the next-node table: at `node`, for `event` (or, when `zero_event_too`, also for event 0), the next node.
struct Row {
  std::uint64_t node;
  std::uint64_t event;
  std::uint64_t next;
  bool zero_event_too;
};

inline constexpr std::size_t kRowCount = 6;
inline constexpr Row kRows[kRowCount] = {
    {0x75014b69ULL, 0x33f5911416f09172ULL, kStop, true},
    {0xf8ab569bULL, 0x33f5911416f09172ULL, kStop, true},
    {0x8935bc5bULL, 0x56ccd1bd99a59d3bULL, kStop, true},
    {0xb0c42820ULL, 0xe32dc7dfd49f50a9ULL, kReleaseTriggerNode, false},
    {0xb0c42820ULL, 0x33f5841b26fb8162ULL, kReleaseTriggerNode, false},
    {0xb61a3584ULL, 0x71b428190a37733eULL, kReleaseTriggerNode, false},
};

/// Whether `event` is one of the three the dispatcher must never deliver to the original.
bool IsTriggerEvent(std::uint64_t event);

/// The event the original is given for `event`: kNoEvent for a trigger event, else `event`.
std::uint64_t EventForOriginal(std::uint64_t event);

/// The next node for (`node`, `event`): the row's when one matches, else `stock` (the game's).
std::uint64_t NextNode(std::uint64_t node, std::uint64_t event, std::uint64_t stock);

/// Registers both hooks with the script hook table. Once. Called from combat.cpp by W-REG (not in this change).
void Register();

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
#endif

}  // namespace nevr_combat::dot_trigger
