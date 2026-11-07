#pragma once

// Combat's spawn orbs and PAUSE (#600, EchoCombat slice 7 PR 7; he_is_the_cat's fix_orb_pause.py, read as text,
// reconstructed here as hooks on the spawn point script, tst_dsn_josh_cbt_utility_player_spawn_point e57332952e1cd478).
//
// The orb decides what it does in node ee90948d (handlespawnaccess, RVA 0x104C0, `u64 (thread)`): it reads the
// replicated symbol spawn_access through the engine's field accessor the script calls at slot RVA 0x411A0, for the
// field id at 0x40B64, loaded as a dword (`mov edx, dword [rip]`, 8b 15, at 0x1050E), into a qword (`mov r8, [rsp+0x28]`
// at 0x10524, then `cmp r8, -1`), and returns it when it is pre_match, team_room, 541c3aa06a228bee or main_loop, else
// exec_default. His patch acts right after that read: the match is live (RULES mailbox +0x2F) when the value is set
// (not -1) and is not pre_match, which VeilNet uses for the podium's TEAM READY -> PAUSE; and while PAUSE (+0x2E) is 1,
// the value is taken as pre_match, so the orbs give the players the pre-match orbs (2, the resume countdown, leaves the
// round's orbs so players can spawn back in).
//
// Here the node is wrapped to mark the thread, and the slot is replaced by one that calls the engine and, inside the
// node and for that field only, does the same two things to the qword the engine wrote. The accessor's other callers
// in the script are untouched.

#include <cstdint>

#include "runtime/combat/rules_state.h"

namespace nevr_combat::orb_pause {

inline constexpr std::uint64_t kScript = 0xe57332952e1cd478ULL;
inline constexpr std::uint32_t kNodeRva = 0x104C0;
inline constexpr std::uint32_t kAccessorSlotRva = 0x411A0;
inline constexpr std::uint32_t kSpawnAccessFieldIdRva = 0x40B64;

inline constexpr std::uint64_t kPreMatch = 0x6f6337a43f136354ULL;  // CSymbol64("pre_match")
inline constexpr std::uint64_t kUnset = ~std::uint64_t{0};

// `sub rsp, 0x378; mov rax, 0x9de606e496cee8a3` (spawn_access): the node's first bytes.
inline constexpr std::uint8_t kNodePrologue[] = {0x48, 0x81, 0xec, 0x78, 0x03, 0x00, 0x00, 0x48, 0xb8,
                                                 0xa3, 0xe8, 0xce, 0x96, 0xe4, 0x06, 0xe6, 0x9d};

using AccessorFn = std::uint64_t (*)(void* ctx, std::uint64_t field, void* out);

/// Whether the match is live for a spawn_access value: set, and not pre_match.
bool Live(std::uint64_t spawn_access);

/// The value the orb acts on: pre_match while PAUSE is on, else the one read.
std::uint64_t Acted(std::uint64_t spawn_access, rules::Pause paused);

/// What the replaced accessor does around the engine's: inside the node and for the spawn_access field (compared on the
/// low dword: the id is a dword), it publishes Live() to `rules` and writes Acted() back over the qword. Exposed for tests.
std::uint64_t ReadField(AccessorFn engine, std::uint32_t spawn_field, bool in_node, const rules::RulesState& rules,
                        void* ctx, std::uint64_t field, void* out);

/// Registers the hook and the slot with the script hook table. `mailbox` is the RULES mailbox (game base +
/// rules::kMailboxRva). Once.
void Register(volatile std::uint8_t* mailbox);

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
#endif

}  // namespace nevr_combat::orb_pause
