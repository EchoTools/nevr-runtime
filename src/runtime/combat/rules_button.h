#pragma once

// The podium terminal's RULES button works in Combat private matches (#600, EchoCombat slice 7 PR 11a; he_is_the_cat's
// fix_rules_terminal.py, read as text, its button half, reconstructed here as slot replacements on the private-match
// podium terminal script, d4653fc2065e8184).
//
// The terminal (READY / RESTART / CHANGE TEAM / SPECTATE / RULES) reads the game's matchisarena and matchiscombat
// flags 14 times, and its button logic follows them, so in a Combat match the RULES button stays grey. Each read loads
// the field's id as a dword (`mov edx, dword [rip]`, the ids at 0x3E250 and 0x3E7EC, stored by setup_bindings), points
// r8 at a byte, and calls the engine's flag accessor through a slot: matchisarena through 0x3E748 (4 calls) and
// matchiscombat through 0x3E868 (10 calls), each slot called for that one field only. Seven reads land in the graph's
// data (+0x169 arena, twice; +0x16A combat, five times), and seven in a stack byte of the calling function, which it
// then tests (+0xC9 arena at 0x31C81 and 0x3383C; +0xCA combat at 0x31C9B and 0x33856, tested by `cmpb` at 0x31D01;
// +0x19A combat at 0x2FD78, 0x30DC8 and 0x34670). His patch replaces each call with `mov byte [r8], 1` (arena) or `0`
// (combat), so the terminal always reads "Arena, not Combat" and behaves as in Arena; the match stays Combat. Two of
// the reads feed the SPECTATE button's unlock check, which then follows arena_spectate_unlocked.
//
// Here each slot is replaced by one that calls the engine and then, for its field, writes that byte: one byte, as each
// destination is a one-byte flag (in the data, +0x169 and +0x16A are adjacent). The RULES page's rows themselves are Arena's until the later PRs make them Combat's.

#include <cstdint>

namespace nevr_combat::rules_button {

inline constexpr std::uint64_t kScript = 0xd4653fc2065e8184ULL;
inline constexpr std::uint32_t kArenaSlotRva = 0x3E748;
inline constexpr std::uint32_t kArenaFieldIdRva = 0x3E250;
inline constexpr std::uint32_t kCombatSlotRva = 0x3E868;
inline constexpr std::uint32_t kCombatFieldIdRva = 0x3E7EC;

using AccessorFn = std::uint64_t (*)(void* ctx, std::uint64_t field, void* out);

/// What a replaced accessor does: the engine's call, then, for `field_id` (compared on the low dword: the id is a
/// dword), `value` written to the byte at `out`. When it writes, `engine_byte` (if given) gets the byte the engine
/// wrote there first.
std::uint64_t ReadFlag(AccessorFn engine, std::uint32_t field_id, std::uint8_t value, void* ctx, std::uint64_t field,
                       void* out, int* engine_byte = nullptr);

/// Registers the two slots with the script hook table. Once.
void Register();

#ifdef NEVR_TEST_HOOKS
void ResetForTest();
/// Clears both slots' module, as a read racing the script's free would see it.
void ForgetModulesForTest();
#endif

}  // namespace nevr_combat::rules_button
